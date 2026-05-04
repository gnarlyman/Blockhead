# Blockhead-Reborn Implementation Plan

**Mission:** Fork shadeMe/Blockhead and ship a hardened version that fixes the cross-thread alloc-swap-free race in the head/face FaceGen pipeline. This bug crashes Oblivion when mounted-actor NPCs (e.g. OOO Imperial Legion patrols) stream their 3D in. Bug is reproducible on a near-vanilla setup and likely affects every large modlist using Blockhead with OOO/MMM/MOO.

**Status entering this session:** build pipeline working, instrumented build deployed, retry-loop guard implemented but proven insufficient (single SwapFaceGenHeadData call still crashes via dtor race). Now we fix it properly.

---

## Required reading (in this order, before touching code)

These are the load-bearing context files. Skim memory first, then dive into the source.

1. **Memory:**
   - `~/.claude/projects/D--Modlists/memory/feedback_blockhead_mounted_actor_crash.md` — full bug analysis
   - `~/.claude/projects/D--Modlists/memory/feedback_blockhead_build_setup.md` — build environment recipe
   - `~/.claude/projects/D--Modlists/memory/reference_blockhead_source.md` — code structure map
   - `~/.claude/projects/D--Modlists/memory/project_blockhead_reborn.md` — strategic context

2. **Source files (in this clone):**
   - `HeadOverride.cpp:411-700` — `SwapFaceGenHeadData` (the buggy alloc/swap pattern)
   - `HeadOverride.cpp:703-744` — `DoFaceGenHeadParametersDtorHook` (the buggy free pattern)
   - `HeadOverride.cpp:648-689` — `DoTESRaceGetFaceGenHeadParametersHook` (with the inadequate retry guard from prior session)
   - `BlockheadInternals.cpp:255` — `TESModel::DeleteInstance` (calls FormHeap_Free)
   - `BlockheadInternals.cpp:382` — `FormHeap_Free` (the actual free that crashes)

3. **Reference for fix pattern:**
   - shadeMe's commit `3379950` (2021-08-21) on this repo — the analogous fix for BodyOverride. Run `git show 3379950 -- BodyOverride.cpp` to see the path-indexed `unordered_map` pattern. Study the diff before designing.

---

## Sanity check first (10 min, optional but recommended)

Confirm the test infrastructure still works before changing code:

```powershell
# Verify the build still works as-is
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\Modlists\_clones\Blockhead\Blockhead.sln" /p:Configuration=Release /p:Platform=Win32 /v:minimal /nologo

# Verify the DLL exists (post-build copy step will fail with MSB3073 — ignore)
Get-Item "D:\Modlists\_clones\Blockhead\Release\Blockhead.dll" | Select-Object Name, Length, LastWriteTime
```

If MSBuild can't find the v142 toolset, the user may need to install it (or retarget to v143/v144 in `Blockhead.vcxproj` — single-line PlatformToolset change per Configuration block).

---

## The 6 Fixes (execution order matters)

Implement and test each in sequence. After each fix, **build + deploy + run the repro test**. Don't bundle multiple fixes into one test — we need to know which one moves the needle.

### Fix 1: Don't NULL-out pathless models (lowest-risk experiment first)

**Hypothesis:** the "sanity check" at HeadOverride.cpp:442-451 is what triggers the engine to retry. If we leave entries alone instead of NULLing them, the engine may consider the FaceGenParams complete after one swap.

**Change:** comment out lines 442-451, or wrap in `#if 0`. That's it.

**Test:** rebuild, deploy, run repro. Check `Blockhead.log` for `[RBRN] RETRY-LOOP` lines. If gone, we've identified the engine's "retry trigger" — and Fix 2 may be less critical.

**Risk:** low. The original code was a defensive sanity check; removing it might cause asset issues but not crashes. If asset issues appear, restore.

### Fix 2: Allocator ownership tracking

**Hypothesis:** `DoFaceGenHeadParametersDtorHook` calls `FormHeap_Free` on every non-NULL entry in `FaceGenParams->models.data[i]`. Some of those pointers were placed there by Blockhead (allocated via `FormHeap_Allocate` — safe to free), some by the engine (allocated via something else — wrong-allocator free → crash).

**Change in `HeadOverride.cpp`:** add a per-FGP set of pointers Blockhead allocated:

```cpp
// At top of HeadOverride.cpp, near other namespace-scope statics:
static std::mutex                                                       g_OwnedPointersLock;
static std::unordered_map<FaceGenHeadParameters*, std::unordered_set<void*>> g_OwnedPointers;

static void TrackOwnedPointer(FaceGenHeadParameters* fgp, void* ptr) {
    if (!fgp || !ptr) return;
    std::lock_guard<std::mutex> g(g_OwnedPointersLock);
    g_OwnedPointers[fgp].insert(ptr);
}

static bool ConsumeOwnedPointer(FaceGenHeadParameters* fgp, void* ptr) {
    if (!fgp || !ptr) return false;
    std::lock_guard<std::mutex> g(g_OwnedPointersLock);
    auto fgpIt = g_OwnedPointers.find(fgp);
    if (fgpIt == g_OwnedPointers.end()) return false;
    auto erased = fgpIt->second.erase(ptr) > 0;
    if (fgpIt->second.empty()) g_OwnedPointers.erase(fgpIt);
    return erased;
}

static void DropAllOwnedFor(FaceGenHeadParameters* fgp) {
    if (!fgp) return;
    std::lock_guard<std::mutex> g(g_OwnedPointersLock);
    g_OwnedPointers.erase(fgp);
}
```

**In `SwapFaceGenHeadData` (HeadOverride.cpp:~468 and ~525)** — after each `CreateInstance()` succeeds and the pointer is about to be swapped into `FaceGenParams`, call `TrackOwnedPointer(FaceGenParams, NewModel)` (and same for `NewTexture`).

**In `DoFaceGenHeadParametersDtorHook` (HeadOverride.cpp:703)** — change the free loop:
```cpp
// BEFORE (current):
if (SneakyBugger)
    InstanceAbstraction::TESModel::DeleteInstance(SneakyBugger);

// AFTER:
if (SneakyBugger && ConsumeOwnedPointer(FaceGenParams, SneakyBugger))
    InstanceAbstraction::TESModel::DeleteInstance(SneakyBugger);
// else: pointer was either never tracked (engine-owned) or already freed — leave alone
```

This single change eliminates both **wrong-allocator free** and **double-free** failure modes simultaneously: a pointer not in our set is never freed; a pointer freed once is removed from the set so subsequent dtor calls treat it as engine-owned.

**Memory note:** consider clearing the FGP's entry from `g_OwnedPointers` if it becomes empty (already handled in `ConsumeOwnedPointer`). FGP buffers are recycled by the engine, so we DO want stale entries cleaned up. If you observe slow memory growth in long sessions, add periodic cleanup based on FGP heuristics.

**Test:** rebuild, deploy, run repro. Should eliminate the `Blockhead!FormHeap_Free+0x18` crash signature observed with our retry guard.

### Fix 3: Per-FGP critical section

**Hypothesis:** even with allocator tracking, BSTask thread can read `FaceGenParams->models.data[i]` at the exact instant main thread is between "free old, write new" steps. For one machine instruction's worth, the pointer is stale/freed.

**Change in `HeadOverride.cpp`:** add a per-FGP lock that wraps the whole swap operation AND the dtor operation. Same map-of-locks pattern as Fix 2:

```cpp
// At namespace scope:
static std::mutex                                              g_FGPLocksMutex;
static std::unordered_map<FaceGenHeadParameters*, std::mutex>  g_FGPLocks;

static std::mutex& GetFGPLock(FaceGenHeadParameters* fgp) {
    std::lock_guard<std::mutex> g(g_FGPLocksMutex);
    return g_FGPLocks[fgp];  // default-constructs on miss
}
```

**Wrap `SwapFaceGenHeadData`:**
```cpp
void SwapFaceGenHeadData(TESRace* Race, FaceGenHeadParameters* FaceGenParams, TESNPC* NPC, bool FixingFaceNormals) {
    if (!FaceGenParams) return;
    std::lock_guard<std::mutex> fgpLock(GetFGPLock(FaceGenParams));

    _MESSAGE("[RBRN] SwapFaceGenHeadData enter NPC=%08X ...", ...);
    // existing body
}
```

**Wrap `DoFaceGenHeadParametersDtorHook`:**
```cpp
void __stdcall DoFaceGenHeadParametersDtorHook(FaceGenHeadParameters* FaceGenParams) {
    if (!FaceGenParams) return;
    std::lock_guard<std::mutex> fgpLock(GetFGPLock(FaceGenParams));

    // existing body
    DropAllOwnedFor(FaceGenParams);  // also clean up the ownership map after dtor
}
```

**Cleanup:** the `g_FGPLocks` map will accumulate entries forever (one per ever-seen FGP). Real impact is probably negligible since the engine recycles a small handful of FGP buffers (we observed only 2 unique pointers across thousands of operations). Optionally, periodically drop entries with 0 active locks. Skip optimization until proven necessary.

**Test:** rebuild, deploy, run repro. Should eliminate the cross-thread race window entirely.

**Performance note:** uncontended std::mutex on Windows is a SRWLOCK, ~10-30ns per acquire. Negligible at FaceGen frequencies.

### Fix 4: Keep the retry-loop guard

The retry guard (already implemented at `DoTESRaceGetFaceGenHeadParametersHook` line 648-689) doesn't fix the root cause but it's defense in depth. Even if Fixes 1+2+3 work, the engine retry behavior may still be pathological for some NPC data states. Keep the guard as belt-and-suspenders.

**Optional refinement:** the current guard only stores `(NPC*, FGP*)` for one slot. If the engine alternates between two NPCs in a tight loop, the guard would miss it. Consider expanding to a small bounded set (last 4 tuples) if observed in logs. Don't over-engineer prematurely.

### Fix 5: Same treatment for SwapBodyData (audit only)

`BodyOverride.cpp` already received the Peryite fix in `3379950`. Audit it to make sure no analogous unfixed swap+free pattern remains. If it's clean, no change needed. If it has the same issue, apply Fixes 2+3 there too with the same primitives (the helper functions are reusable).

### Fix 6: Versioning

Update `BuildInfo.h` and bump `VERSION_MINOR` to indicate fork status:
```cpp
// VersionInfo.h line 15: change VERSION_MINOR from 1 to 2
#define VERSION_MAJOR               11
#define VERSION_MINOR               2  // fork: 11.2.x = Blockhead-Reborn
```

Update `BuildInfo.h` revision/build to match build day. Optional: add a "Blockhead-Reborn" string to startup banner so users can verify they're running our fork.

---

## Verification protocol

After all fixes are deployed, the **must-pass** test:

1. Reborn-OOO profile with minimum stack + Blockhead enabled
2. Load any save in Cyrodiil
3. `coc DerelictMineExterior`
4. Wait 1-3 in-game hours (T key) until mounted Imperial Legion patrol streams in
5. **Run for at least 5 minutes wall time** (the prior crash window was 1:40 - 3:30)
6. Move around, fight a goblin, leave the cell, come back

**Success criteria:**
- No crash log generated in `D:\Modlists\Reborn\overwrite\Root\Crash Logs\` from this session
- `Blockhead.log` contains `[RBRN] HOOK DoTESRaceGetFaceGenHeadParametersHook NPC=000700CC` lines (proving the patrol streamed in and the hook fired)
- `[RBRN] RETRY-LOOP` lines may appear once or twice but should not flood
- Game runs at normal FPS (no hot-loop FPS tank)

**Document failure mode if it occurs:**
- Read latest crash log: `Get-ChildItem "D:\Modlists\Reborn\overwrite\Root\Crash Logs\" | Sort LastWriteTime -Descending | Select -First 1 | Get-Content`
- Tail Blockhead.log: `Get-Content "D:\Modlists\Reborn\overwrite\Root\Blockhead.log" -Tail 30`
- Compare crash signature against the two known patterns (vanilla `Set3D+0x48` chain vs. `FormHeap_Free` chain). If a third signature appears, that's a new bug class — investigate before continuing.

**Regression check (optional but recommended):** load a save where Blockhead currently works (unmounted city NPC with OCOv2 face overrides). Confirm faces still render correctly — i.e., the override pipeline didn't break.

---

## Build + deploy commands

Reference, full pipeline:

```powershell
# Build
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild "D:\Modlists\_clones\Blockhead\Blockhead.sln" /p:Configuration=Release /p:Platform=Win32 /v:minimal /nologo

# Deploy (post-build copy step always fails — use this manually)
Copy-Item -Force "D:\Modlists\_clones\Blockhead\Release\Blockhead.dll" "D:\Modlists\Reborn\mods\Blockhead\OBSE\Plugins\Blockhead.dll"

# Verify deployed version
(Get-Item "D:\Modlists\Reborn\mods\Blockhead\OBSE\Plugins\Blockhead.dll").VersionInfo | Format-List FileVersion

# Clear stale Blockhead.log before testing (optional, makes log analysis clean)
$log = "D:\Modlists\Reborn\overwrite\Root\Blockhead.log"
if (Test-Path $log) { Remove-Item $log -Force }
```

The MSBuild command **will exit with code 1** because the `OblivionPath` macro is undefined and the post-build `copy` command fails. The DLL itself builds successfully — verify by checking for `Release\Blockhead.dll` with a recent `LastWriteTime`.

Original shipped DLL is backed up at `D:\Modlists\Reborn\mods\Blockhead\OBSE\Plugins\Blockhead.dll.shipped-11.1.5.1221.bak`. Restore by overwriting the deployed file.

---

## After fixes work

Open a fork on GitHub:

```bash
cd D:\Modlists\_clones\Blockhead
# (Currently this points at shadeMe/Blockhead origin and has dirty working tree from prior session)
git remote rename origin upstream
git remote add origin git@github.com:gnarlyman/Blockhead-Reborn.git
git checkout -b feat/concurrent-safety
```

Then:
1. Squash the build-system changes (junctions etc. don't need to commit; EnvVars.props edit does)
2. Stage the source fixes as a coherent commit: "Fix concurrent-modification bug in FaceGen head override pipeline"
3. Push
4. Cut a v11.2.0 release with binaries
5. (Optional) Nexus page

---

## Known dead-ends (don't waste time re-exploring)

- **Bisecting APW vs Reborn mod stacks** — bug is in shipped Blockhead source, configuration-independent. APW is "stable" only because casual play doesn't trigger the scenario.
- **Tweaking face data quantity** — tested with full OCOv2 + Seamless + everything, doesn't matter
- **Threading INI tweaks** (`iNumHavokThreads`, `iThreads`) — verified they don't affect the bug
- **ORC config** — extensively tested, not the cause
- **Adding `IsRidingHorse` guard** — Blockhead doesn't call `Update3D` on NPCs (only on player at BodyOverride.cpp:327), so the prior memory's recommended fix doesn't apply
- **Disabling specific patrol ACHRs** — extends time-to-crash but doesn't fix it; the bug fires on any sufficiently-streamed mounted NPC

---

## Open questions to investigate (only if time permits)

These would deepen understanding but aren't required for the fix:

1. **Why does the engine retry-loop on `000700CC` specifically?** Other patrol bases like `000700CD`, `000700C0` process cleanly. What's special about VirtueRider? Likely something in the TESNPC face data — empty model path, missing FaceGen data, race-component combo. Inspecting via xEdit would tell us. Not required to fix.

2. **Does APW actually have the bug?** One launch of APW with the same `coc DerelictMineExterior + wait` test would settle the "is APW immune or just untriggered" question. If APW crashes too, the prior "what makes APW different" memory framing was wrong on a fundamental premise — and our fix benefits the whole community.

3. **Does the Skyrim OverlayFix approach (replace `Load3D + Set3D` with single `Set3DHook`) translate to Oblivion?** Higher-effort engine-level fix. May be worth doing as a follow-up if our Blockhead fork still has edge cases.

---

## Acceptance criteria for v11.2.0 release

- [ ] All 6 fixes implemented (or documented why a fix was skipped)
- [ ] Repro test (mounted patrol at DerelictMine) passes for 10+ min wall time without crash
- [ ] No regression on city NPCs (OCOv2 faces still render)
- [ ] `Blockhead.log` shows expected hook firings without retry-loop floods
- [ ] Build deterministic from clean checkout + repository setup steps in `feedback_blockhead_build_setup.md`
- [ ] CHANGELOG documenting the fix and credit to shadeMe for the original
- [ ] LICENSE preserved (whatever shadeMe used)
- [ ] README updated with fork notice and link back to upstream
