#include "EngineRaceFix.h"
#include "BlockheadInternals.h"

#include <windows.h>
#include "Detours/detours.h"

#pragma comment(lib, "Detours/detours.lib")

namespace EngineRaceFix
{
	// =============================================================================
	// [RBRN] BSFaceGen FGP-corruption-tolerant fix stack.
	//
	// Layered fixes addressing successive crash signatures observed during the
	// 2026-05-04 investigation:
	//
	//   1. LFM bucket-array FormHeapFree NOPs (refined Option A) — kills the
	//      original sub_432C30+0x44 LFM bucket-array UAF.
	//
	//   2. sub_52DED0 chokepoint mutex — serializes worker face-load chain.
	//      Killed the NiObjectNET::SetName+0x4 relocation crash.
	//
	//   3. AgeMorphTable validation-failure redirect — replaces call to
	//      _invalid_parameter at 0x006EDDD4 with jmp to existing "return 0.0"
	//      early-exit path at 0x006EDD8F. Killed the empty-morph-vector crash.
	//
	//   4. BSFaceGen_DoSomethingWithFaceGenNode FGP-validation hook (NEW) —
	//      catches null FGP.models.data (ebp+0x78) and similar early null fields,
	//      bails out cleanly via skip path. Targets the AV at +0x179.
	//
	// All fixes target the corrupted FaceGenHeadParameters bug class. The FGP
	// passed to worker face-load chain has critical fields zero/garbage when
	// streaming patrol NPCs under OCO. Each layer either validates the data
	// before the engine reads it, or reroutes the failure to a graceful path.
	// =============================================================================

	// LFM bucket-array FormHeapFree NOPs (refined Option A).
	_DefineNopHdlr(BucketArrayFreeChainA, 0x0043296E, 5);
	_DefineNopHdlr(BucketArrayFreeChainB, 0x004327EC, 5);

	// sub_52DED0 worker face-load chokepoint mutex.
	typedef void(__thiscall* fn_sub_52DED0)(void* self, void* a1, void* a2, void* a3, void* a4, void* a5);
	static fn_sub_52DED0 orig_sub_52DED0 = (fn_sub_52DED0)0x0052DED0;

	// sub_435300 = QueuedHead::Run — the worker function that processes face-gen
	// for a single NPC. We hook it to install a sentinel BSFaceGenNiNode for bad
	// actors so the engine stops re-queueing them.
	typedef void(__thiscall* fn_sub_435300)(void* self);
	static fn_sub_435300 orig_sub_435300 = (fn_sub_435300)0x00435300;
	static volatile LONG s_qhSentinelCount = 0;

	// sub_522260 = standalone TESNPC.face0/face1 release-only function. Called from
	// sub_4B31D0 (NPC cleanup, ~per-frame for non-player NPCs). Each call clears
	// face0/face1 to NULL even if our sentinel was installed — defeats v541's
	// sentinel approach. v542: hook this and skip for bad actors so sentinel survives.
	typedef void(__thiscall* fn_sub_522260)(void* self);
	static fn_sub_522260 orig_sub_522260 = (fn_sub_522260)0x00522260;
	static volatile LONG s_releaseSkipCount = 0;

	// sub_528D90 = main-thread per-frame face-gen processor. RE'd 2026-05-05:
	// - `this` = TESNPC*
	// - iterates TESNPC.unk1 (+0x108)
	// - for each unprocessed item (vtable[0x4C/0x4D] both return 0), constructs
	//   stack-local QueuedHead + runs sub_435300 inline
	// - called from sub_665260 (15-caller chain, per-frame render-related)
	// - v544 measured: didn't actually fire — not the storm driver
	typedef void(__thiscall* fn_sub_528D90)(void* self);
	static fn_sub_528D90 orig_sub_528D90 = (fn_sub_528D90)0x00528D90;
	static volatile LONG s_528D90SkipCount = 0;

	// sub_4348B0 = LFM doubly-linked queue insert. The actual enqueue site for
	// async QueuedHead tasks. RE'd 2026-05-05:
	// - 8 callers (mostly task-related)
	// - signature: __thiscall(this=queue, arg1=task*) returning queue
	// - the agent confirmed skipping is safe (caller stores ret val but no error path)
	// v545: hook entry, read [arg+0x20] (= QueuedHead.npc if QueuedHead), skip
	// enqueue for bad actors. Prevents async LFM queue accumulation → eliminates
	// the iteration UAF source (sub_4328B0/sub_433BC0 crashes on stale queue items).
	typedef void*(__thiscall* fn_sub_4348B0)(void* self, void* task);
	static fn_sub_4348B0 orig_sub_4348B0 = (fn_sub_4348B0)0x004348B0;
	static volatile LONG s_4348B0SkipCount = 0;

	// Forward declarations (definitions further below).
	static bool IsBadActor(void* tesnpc);
	extern const UInt32 kBadActorLocalFormIDs[];
	extern const UInt32 kBadActorCount;

	// v560: bypass the bad-actor blacklist short-circuits at all 4 hook sites.
	// The blacklist (Layer 6, v541) was a FaceGen-give-up workaround for the LFM
	// cancellation crash; v557+v558 fix that crash at the actual race site, so the
	// give-up workaround now only HIDES faces that would otherwise load fine
	// (Layers 1-5: LFM NOPs, sub_52DED0 mutex, AgeMorphTable redirect, DoSomething
	// FGP validator, sub_5547F0 eye-validity patches still cover the FaceGen-side
	// safety). Set false to restore v558 behavior.
	static const bool kBypassBadActorLogic = true;

	// =============================================================================
	// [RBRN] v541 — BSFaceGenNiNode sentinel for storm-stop.
	//
	// Problem (per logs ending v540): bad actors produce empty B1/B2 → engine never
	// assigns to TESNPC.face0/face1 → engine re-queues QueuedHead at render-frame
	// rate (~10/sec/visible bad actor) → LFM hazard-pointer protocol stress in
	// sub_433BC0/sub_4328B0 → eventual UAF crash.
	//
	// Fix: pre-allocated sentinel BSFaceGenNiNode-shaped struct with fake no-op
	// vtable and INT_MAX/2 refcount. Hook QueuedHead::Run entry; for bad actors,
	// write &g_sentinel into TESNPC.face0/face1 (if currently NULL) and skip the
	// actual face-gen work. Set3D then sees non-NULL face nodes and stops queueing
	// for that NPC. Storm dies at its source.
	//
	// Refcount stays sky-high so InterlockedDecrement releases never destroy it.
	// Vtable methods are all no-ops, so any engine vtable dispatch is safe.
	// Direct field reads on the sentinel see zero (= NULL pointers / empty data),
	// which the engine's existing NULL-skip paths handle gracefully.
	// =============================================================================
	#pragma pack(push, 4)
	struct FaceGenSentinel {
		void* vtbl;             // +0x00
		LONG  refcount;         // +0x04
		UInt8 padding[0x200];   // generous coverage of BSFaceGenNiNode size
	};
	#pragma pack(pop)
	static FaceGenSentinel g_sentinel;

	// Fake vtable — 64 entries, all pointing to a no-op function.
	static const UInt32 kSentinelVtblSize = 64;
	static void* g_sentinel_vtbl[kSentinelVtblSize];

	// Vtable thunk arity matters! __thiscall callers on x86 pass implicit `this`
	// in ECX and additional args on the stack; the CALLEE pops the stack args.
	//
	// vtable[0] = virtual void Destroy(bool noDealloc) — 1 stack arg → `ret 4`
	// vtable[other] = unknown arity (we don't have the BSFaceGenNiNode signatures)
	//
	// Using a wrong arity corrupts the stack. v542 used `ret` for everything which
	// caused the destructor's `push 1` arg to leak onto the caller's stack →
	// subsequent reads off-by-4 → eventual NULL function call.
	//
	// v543 strategy: vtable[0] uses `ret 4` (correct for destructor). All other
	// slots use `ret 4` too — most BSFaceGenNiNode-ish virtual methods take 0-1
	// args; if any take more, we'll see a new crash signature and add specific
	// thunks. Most likely the engine NEVER calls slots other than [0] on a stub
	// face node, since real usage requires populated child structures.
	__declspec(naked) static void sentinel_dtor_thunk()
	{
		__asm { ret 4 }   // virtual Destroy(bool) — pop 1 arg
	}
	__declspec(naked) static void sentinel_unary_thunk()
	{
		__asm { ret 4 }   // best-effort for 1-stack-arg methods
	}
	__declspec(naked) static void sentinel_nullary_thunk()
	{
		__asm { ret }     // for 0-arg methods (rare in BSFaceGenNiNode)
	}

	static void InitSentinel()
	{
		memset(&g_sentinel, 0, sizeof(g_sentinel));
		// vtable[0] is the destructor (Destroy(bool)) — `ret 4`
		g_sentinel_vtbl[0] = (void*)&sentinel_dtor_thunk;
		// Slots [1..N] — best-effort `ret 4`. If we see new crashes from wrong
		// arity, we can refine specific slots.
		for (UInt32 i = 1; i < kSentinelVtblSize; i++) {
			g_sentinel_vtbl[i] = (void*)&sentinel_unary_thunk;
		}
		g_sentinel.vtbl = g_sentinel_vtbl;
		g_sentinel.refcount = 0x40000000;  // ~1 billion; can't be decremented to zero
		_MESSAGE("[RBRN] FaceGen sentinel initialized at %p (vtbl at %p, refcount=%ld)",
		         &g_sentinel, g_sentinel_vtbl, g_sentinel.refcount);
	}

	// hook_sub_522260: standalone face0/face1 release. Skip for bad actors so
	// our sentinel survives between QueuedHead::Run calls.
	static void __fastcall hook_sub_522260(void* self, void* /*edx*/)
	{
		if (!kBypassBadActorLogic && IsBadActor(self)) {
			LONG n = InterlockedIncrement(&s_releaseSkipCount);
			if (n <= 5 || (n % 100) == 0) {
				_MESSAGE("[RBRN] sub_522260 release skip #%ld for bad actor", n);
			}
			return;
		}
		orig_sub_522260(self);
	}

	// hook_sub_528D90: per-frame face-gen processor. Skip entirely for bad actors
	// so the iteration over unk1 doesn't run → no QueuedHead construct+run cycle →
	// no sub_435300 → no sub_52DED0 → no allocation churn.
	static void __fastcall hook_sub_528D90(void* self, void* /*edx*/)
	{
		if (!kBypassBadActorLogic && IsBadActor(self)) {
			LONG n = InterlockedIncrement(&s_528D90SkipCount);
			if (n <= 5 || (n % 100) == 0) {
				_MESSAGE("[RBRN] sub_528D90 skip #%ld for bad actor", n);
			}
			return;
		}
		orig_sub_528D90(self);
	}

	// VirtualQuery-based safe read — more reliable than __try because the compiler
	// can hoist derefs out of __try blocks. Returns true if the entire range
	// [p, p+bytes) is mapped readable.
	static bool IsValidRead(const void* p, SIZE_T bytes)
	{
		MEMORY_BASIC_INFORMATION mbi = {0};
		if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
		if (mbi.State != MEM_COMMIT) return false;
		DWORD prot = mbi.Protect & 0xFF;
		if (prot == PAGE_NOACCESS) return false;
		if (mbi.Protect & PAGE_GUARD) return false;
		const UInt8* end = (const UInt8*)p + bytes;
		const UInt8* regionEnd = (const UInt8*)mbi.BaseAddress + mbi.RegionSize;
		return end <= regionEnd;
	}

	// hook_sub_4348B0: LFM queue enqueue. The function is called for many task
	// types (8 callers), only one of which constructs QueuedHeads. We filter by
	// vtable check first: only QueuedHead-vtable tasks (0x00A36CE4) get inspected
	// for bad actors. Non-QueuedHead tasks pass through unchanged. All reads use
	// VirtualQuery validation (SEH __try is unreliable due to compiler hoisting).
	static const UInt32 kQueuedHeadVtbl = 0x00A36CE4;

	static void* __fastcall hook_sub_4348B0(void* self, void* /*edx*/, void* task)
	{
		// Type-check the task: must be a QueuedHead (vtable[0] == 0xA36CE4).
		if (!task || !IsValidRead(task, 4)) {
			return orig_sub_4348B0(self, task);
		}
		UInt32 vtbl = *(const UInt32*)task;
		if (vtbl != kQueuedHeadVtbl) {
			// Not a QueuedHead — different task type, don't filter
			return orig_sub_4348B0(self, task);
		}

		// QueuedHead.npc at +0x20 (per xOBSE GameTasks.h)
		if (!IsValidRead((const char*)task + 0x20, 4)) {
			return orig_sub_4348B0(self, task);
		}
		void* npc = *(void* const*)((const char*)task + 0x20);
		if (!npc || !IsValidRead((const char*)npc + 0xC, 4)) {
			return orig_sub_4348B0(self, task);
		}

		UInt32 refID = *(const UInt32*)((const char*)npc + 0xC);
		UInt32 local = refID & 0x00FFFFFF;
		for (UInt32 i = 0; i < kBadActorCount; i++) {
			if (local == kBadActorLocalFormIDs[i]) {
				if (kBypassBadActorLogic) break;  // v560: let it enqueue normally
				LONG n = InterlockedIncrement(&s_4348B0SkipCount);
				if (n <= 5 || (n % 100) == 0) {
					_MESSAGE("[RBRN] sub_4348B0 enqueue skip #%ld for bad actor (formID=%08X)",
					         n, refID);
				}
				return self;  // skip enqueue
			}
		}
		return orig_sub_4348B0(self, task);
	}

	// hook_sub_435300: for bad actors, install sentinel into TESNPC.face0/face1
	// and skip the original. For good actors, call orig as normal.
	static void __fastcall hook_sub_435300(void* self, void* /*edx*/)
	{
		if (!self) {
			orig_sub_435300(self);
			return;
		}

		void* npc = NULL;
		__try {
			// QueuedHead.npc is at offset +0x20 (per xOBSE GameTasks.h).
			npc = *(void**)((char*)self + 0x20);
		} __except(EXCEPTION_EXECUTE_HANDLER) {}

		if (kBypassBadActorLogic || !IsBadActor(npc)) {
			orig_sub_435300(self);
			return;
		}

		// Bad actor — install sentinel into TESNPC.face0/face1 if currently NULL.
		// If they're already non-NULL (sentinel from previous install OR a real
		// face from a successful prior call), leave them alone.
		__try {
			void** pFace0 = (void**)((char*)npc + 0x1D4);  // TESNPC.face0
			void** pFace1 = (void**)((char*)npc + 0x1D8);  // TESNPC.face1
			if (*pFace0 == NULL) *pFace0 = &g_sentinel;
			if (*pFace1 == NULL) *pFace1 = &g_sentinel;
		} __except(EXCEPTION_EXECUTE_HANDLER) {}

		LONG n = InterlockedIncrement(&s_qhSentinelCount);
		if (n <= 5 || (n % 100) == 0) {
			_MESSAGE("[RBRN] QueuedHead::Run sentinel-install #%ld for bad actor", n);
		}
		// Don't call orig — task quietly completes without face-gen work.
	}

	// BSFaceGen_DoSomethingWithFaceGenNode FGP-validation hook.
	typedef void (__cdecl* fn_DoSomething)(void* faceGenNode, void* faceGenParams);
	static fn_DoSomething orig_DoSomething = (fn_DoSomething)0x005551C0;

	static CRITICAL_SECTION s_facegenLock;
	static bool             s_installed = false;

	static volatile LONG    s_doSomethingSkipCount = 0;
	static volatile LONG    s_moonpcSkipCount = 0;

	// Empirically identified bad actors via v534-v536 diagnostics (2026-05-05):
	// - MOONpc (Maskar's Oblivion Overhaul "Race Toggler" template). Local formID
	//   0x006FDF7C. Mod index varies per load order (was 0x37/0x38 in our test runs).
	//   Race-toggled at runtime to vanilla races (DarkElf 0x191C1, WoodElf 0x223C8 seen).
	//   sub_52DED0 produces empty B1/B2 → DoSomething AV → LFM retry storm → eventual
	//   sub_433BC0/sub_4328B0 UAF crash. Static record fields look fine; runtime state
	//   is the issue (race toggle leaves face data inconsistent).
	//
	// Strategy: at sub_52DED0 entry, check parent (a3 = TESNPC*). If its local formID
	// matches a known-bad list, return immediately without allocating or invoking the
	// helper chain. The OUT pointers (a1, a2) were initialized to NULL by the caller
	// (sub_435300 at 0x435324/0x43532C); leaving them NULL is the same outcome the
	// engine produces on its own bail paths.
	// Empirically identified bad-actor local formIDs (mod-index-stripped). Each
	// produces empty B1/B2 from sub_52DED0 → DoSomething storm → LFM UAF, OR
	// produces FGP+0xB8/0xBC corruption that Layer 5 catches but only after
	// allocation churn. Skipping at sub_52DED0 entry eliminates both costs.
	//
	// Sources:
	// - 0x006FDF7C: MOO MOONpc (Race Toggler template) — v537 detection
	// - 0x000700CC: vanilla ImperialLegionRiderVirtue — v538 detection
	// - 0x0062C7B5: MOO MOOEasyBanditMeleeFemale04 — v539 detection
	// - 0x000700C0..0x000700CD (sans 0x700CC): vanilla mounted Imperial Legion
	//   patrols (per feedback_oblivion_facegen_corruption_diagnosis.md)
	// - 0x00018BA86, 0x00018BA88, 0x00018BA89: more vanilla mounted patrols
	const UInt32 kBadActorLocalFormIDs[] = {
		0x006FDF7C, 0x0062C7B5,                      // MOO
		0x000700C0, 0x000700C1, 0x000700C2,           // vanilla ILR patrols
		0x000700C3, 0x000700C4, 0x000700C5,
		0x000700C6, 0x000700C7, 0x000700C8,
		0x000700C9, 0x000700CA, 0x000700CB,
		0x000700CC, 0x000700CD,
		0x00018BA86, 0x00018BA88, 0x00018BA89,
	};
	const UInt32 kBadActorCount = sizeof(kBadActorLocalFormIDs) / sizeof(kBadActorLocalFormIDs[0]);

	static bool IsBadActor(void* tesnpc)
	{
		if (!tesnpc) return false;
		UInt32 local = 0;
		__try {
			UInt32 refID = *(UInt32*)((char*)tesnpc + 0xC);
			local = refID & 0x00FFFFFF;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		for (UInt32 i = 0; i < kBadActorCount; i++) {
			if (local == kBadActorLocalFormIDs[i]) return true;
		}
		return false;
	}

	// =============================================================================
	// [RBRN] PROBE — FUN_0043B000 entry observer (v548 diagnostic).
	//
	// CONTEXT: v547's sub_5221C0 probe proved TESRace+0x29C runtime data is FINE
	// for bad actors (identical to good actors). Therefore the empty face arrays
	// don't come from sub_552990's NULL-pair zeroing.
	//
	// The 2026-05-06 agent re-trace then revealed that for NPCs (formType 0x23),
	// `ModelLoader::QueueReference` actually dispatches a `QueuedCharacter` task
	// (vtable 0xA36DDC), NOT a QueuedHead. QueuedCharacter::Run fans out 3 sub-
	// tasks: BODY (FUN_0043D000 reading TESNPC+0xAC), FACE (a child QueuedHead),
	// and EQUIPMENT. The body sub-task's result lands at wrapper+0x2c. THAT is
	// what becomes refr+0x3C — not face0/face1.
	//
	// FUN_0043B000 is the QueuedCharacter completion slot (vtable[14] for the
	// non-helmet variant; QueuedCharacter/Player slot[14] = 0x43B090 calls 0x43B000
	// first). Layout:
	//   wrapper+0x0c = task state (6 = cancelled/done)
	//   wrapper+0x20 = TESObjectREFR* (the bound refr)
	//   wrapper+0x28 = QueuedHead* (face sub-task result)
	//   wrapper+0x2c = body NiNode* (BODY sub-task result — THE critical field)
	//
	// FUN_0043B000 calls FUN_00441EF0(refr, ?, body, 0). Inside FUN_00441EF0:
	//   if (param_3 == 0) param_3 = refr->vtable[0x14c]();   // poly fallback re-queue
	//   else if (refr+0x3C == 0)  refr+0x3C := param_3       // the actual write
	//
	// Hypothesis: for bad actors, body sub-task fails → wrapper+0x2c == NULL →
	// FUN_00441EF0 takes fallback re-queue → refr+0x3C stays NULL → next frame's
	// TESCharacter::Update re-queues → infinite loop → LFM stress → UAF.
	//
	// This probe captures wrapper+0x2c at the moment of completion. If body is
	// NULL for bad actors and non-NULL for good actors, the body sub-task is the
	// real failure point and Blockhead's face-gen layer is irrelevant to refr+0x3C.
	//
	// __thiscall (wrapper in ECX, no stack args). Pure observer, always calls orig.
	// =============================================================================
	typedef void (__fastcall* fn_FUN_0043B000)(void* self, void* /*edx*/);
	static fn_FUN_0043B000 orig_FUN_0043B000 = (fn_FUN_0043B000)0x0043B000;
	static volatile LONG s_probe_43B000_total = 0;
	static volatile LONG s_probe_43B000_logged = 0;

	static void __fastcall hook_FUN_0043B000(void* self, void* /*edx*/)
	{
		LONG total = InterlockedIncrement(&s_probe_43B000_total);

		if (!self || !IsValidRead(self, 0x40)) {
			orig_FUN_0043B000(self, NULL);
			return;
		}

		UInt32 state = *(UInt32*)((char*)self + 0x0C);
		void* refr   = *(void**)((char*)self + 0x20);
		void* face   = *(void**)((char*)self + 0x28);
		void* body   = *(void**)((char*)self + 0x2C);

		UInt32 refrFormID = 0;
		UInt32 refrFlags = 0;
		void*  refrLoaded3D = NULL;
		UInt32 npcFormID = 0;
		bool   isBad = false;

		if (refr && IsValidRead(refr, 0x44)) {
			refrFormID  = *(UInt32*)((char*)refr + 0x0C);
			refrFlags   = *(UInt32*)((char*)refr + 0x08);
			refrLoaded3D = *(void**)((char*)refr + 0x3C);
			void* npc = *(void**)((char*)refr + 0x40);
			if (npc && IsValidRead(npc, 0x10)) {
				npcFormID = *(UInt32*)((char*)npc + 0x0C);
				isBad = IsBadActor(npc);
			}
		}

		const char* badTag = isBad ? "[BAD] " : "";

		// Log: bad actors always; non-bad only if body == NULL OR first 50 calls (baseline).
		bool shouldLog = isBad || (body == NULL) || (total <= 50);
		if (shouldLog) {
			LONG n = InterlockedIncrement(&s_probe_43B000_logged);
			// Throttle within shouldLog: first 100 always; bad-actor every 50; null-body every 20
			bool emit = (n <= 100)
			         || (isBad && (n % 50) == 0)
			         || (body == NULL && (n % 20) == 0);
			if (emit) {
				_MESSAGE("[RBRN] PROBE 43B000 #%ld %srefr=%p NPC=%08X refrFID=%08X "
				         "body=%p face=%p loaded3D=%p flags=%08X state=%u total=%ld",
				         n, badTag, refr, npcFormID, refrFormID,
				         body, face, refrLoaded3D, refrFlags, state, total);
			}
		}

		orig_FUN_0043B000(self, NULL);
	}

	// =============================================================================
	// [RBRN] PROBE — FUN_004D7D10 entry observer (v550 diagnostic).
	//
	// CONTEXT: v549's FUN_0043D000 hook ACCIDENTALLY throttled the storm via probe
	// latency (~6× slower task completion rate vs v548). User's "no crash" was a
	// timing-accidental side effect. Underlying bug unchanged — refr 0x70106 and
	// 0x70107 still showed body=NULL across hundreds of captures.
	//
	// FUN_004D7D10 is the CANONICAL refr+0x3C writer (TESObjectREFR::SetLoaded3D).
	// Every successful body install passes through here — it's the only place that
	// writes refr+0x3C to a non-NULL value. By hooking entry, we observe:
	//  - Which refrs ever get refr+0x3C populated (= body load succeeded)
	//  - Which refrs NEVER appear (= body load never produced anything)
	//  - Sequence: when does refr+0x3C get NULL'd vs populated
	//
	// Signature: __thiscall(refr) with 1 stack arg (newModel).
	// In: in_ECX = refr. param_1 = newModel (NiNode* or NULL).
	// Body: tests refr+0x3C != newModel, decrefs old, stores new, increfs new.
	//
	// Throttle: log every NULL write + every write for tracked refrFIDs (0x70106,
	// 0x70107, MOO recruit range). Sample 1/100 otherwise. Low overhead — no
	// VirtualQuery on hot path.
	// =============================================================================
	typedef void (__fastcall* fn_FUN_004D7D10)(void* refr, void* /*edx*/, void* newModel);
	static fn_FUN_004D7D10 orig_FUN_004D7D10 = (fn_FUN_004D7D10)0x004D7D10;
	static volatile LONG s_probe_4D7D10_total = 0;
	static volatile LONG s_probe_4D7D10_logged = 0;

	static void __fastcall hook_FUN_004D7D10(void* refr, void* /*edx*/, void* newModel)
	{
		LONG total = InterlockedIncrement(&s_probe_4D7D10_total);

		// Cheap reads — no VirtualQuery. The refr is the engine's `this` so it's
		// guaranteed valid by the caller. Trust it.
		UInt32 refrFID = 0;
		void* oldModel = NULL;
		if (refr) {
			refrFID = *(UInt32*)((char*)refr + 0x0C);
			oldModel = *(void**)((char*)refr + 0x3C);
		}

		// Tracked-refr list — the storming ones we care about
		bool tracked = false;
		switch (refrFID) {
		case 0x00070106:
		case 0x00070107:
		case 0x000700C0: case 0x000700C1: case 0x000700C2: case 0x000700C3:
		case 0x000700C4: case 0x000700C5: case 0x000700C6: case 0x000700C7:
		case 0x000700C8: case 0x000700C9: case 0x000700CA: case 0x000700CB:
		case 0x000700CC: case 0x000700CD:
			tracked = true;
			break;
		}

		bool nullWrite = (newModel == NULL);
		bool shouldLog = nullWrite || tracked || (total <= 30) || (total % 100 == 0);
		if (shouldLog) {
			LONG n = InterlockedIncrement(&s_probe_4D7D10_logged);
			const char* tag = tracked ? "[TRACKED] " : (nullWrite ? "[NULL-WR] " : "");
			_MESSAGE("[RBRN] PROBE 4D7D10 #%ld %srefrFID=%08X newModel=%p oldModel=%p total=%ld",
			         n, tag, refrFID, newModel, oldModel, total);
		}

		orig_FUN_004D7D10(refr, NULL, newModel);
	}

	// =============================================================================
	// [RBRN] PROBE — FUN_004E0F80 (Set3D) entry+exit observer (v554 diagnostic).
	//
	// CONTEXT: v553 captured 4 [TRACKED] writes in FUN_004D7D10. Body NIF DID
	// install for storm refrs (0x70106 newModel=8601943C, 0x70107 newModel=84B85BE0)
	// — then within 5 calls, refr+0x3C was back to NULL. The "clear" entries showed
	// oldModel=NULL — meaning something OTHER than FUN_004D7D10 cleared refr+0x3C
	// between install and our next observation.
	//
	// FUN_004E0F80 (TESObjectREFR::Set3D) at line 838 does a direct
	// `in_ECX[0xf] = 0;` clear — bypassing the canonical writer mutex. If Set3D
	// fires for storm refrs after their install, this is what's cycling them back
	// to NULL → triggering re-queue → storm.
	//
	// Probe captures Set3D entry + exit for tracked refrs only. If we see Set3D
	// called with refr+0x3C transitioning non-NULL→NULL for a storm refr, that's
	// the smoking gun.
	//
	// Signature: __thiscall(refr) with 1 stack arg (newModel). Same as FUN_004D7D10.
	// =============================================================================
	typedef void (__fastcall* fn_FUN_004E0F80)(void* refr, void* /*edx*/, void* newModel);
	static fn_FUN_004E0F80 orig_FUN_004E0F80 = (fn_FUN_004E0F80)0x004E0F80;
	static volatile LONG s_probe_4E0F80_total = 0;
	static volatile LONG s_probe_4E0F80_logged = 0;

	// =============================================================================
	// [RBRN] FIX v558: Block FUN_004D6BF0 (direct Detach3D clearer) for tracked refrs.
	//
	// CONTEXT: v557 prevented Set3D cancellation. Body NIF installed twice for storm
	// refrs (PROBE 4D7D10 #2267-2268 newModel=non-NULL). But the storm continued
	// (5227 SKIP fires). Something else cleared refr+0x3C between install and the
	// next Set3D call.
	//
	// FUN_004D6BF0 (seg_004d0000.c:4924): direct clearer with refcount management:
	//     *(in_ECX + 0x38) = 0x3f800000;  // float 1.0f at +0x38
	//     if (... && refr+0x3C != NULL) {
	//         InterlockedDecrement(*(refr+0x3C) + 1);
	//         if (refcount == 0) (vtbl[0])(1);  // dtor
	//         *(refr + 0x3c) = 0;  // <-- DIRECT CLEAR
	//     }
	//
	// This bypasses both Set3D and FUN_004D7D10 — explains why our v557 install was
	// undone without showing in our probes.
	//
	// Fix: skip for tracked refrs to preserve installed body.
	// Risk: legitimate detaches (refr disable) won't decref the old NiNode → leak.
	//       Tolerable for diagnostic purposes.
	// =============================================================================
	typedef void (__fastcall* fn_FUN_004D6BF0)(void* refr, void* /*edx*/);
	static fn_FUN_004D6BF0 orig_FUN_004D6BF0 = (fn_FUN_004D6BF0)0x004D6BF0;
	static volatile LONG s_4D6BF0_skip = 0;
	static volatile LONG s_4D6BF0_total = 0;

	static void __fastcall hook_FUN_004D6BF0(void* refr, void* /*edx*/)
	{
		LONG total = InterlockedIncrement(&s_4D6BF0_total);

		UInt32 refrFID = 0;
		bool tracked = false;
		void* loaded3DBefore = NULL;
		if (refr) {
			refrFID = *(UInt32*)((char*)refr + 0x0C);
			loaded3DBefore = *(void**)((char*)refr + 0x3C);
			switch (refrFID) {
			case 0x00070106:
			case 0x00070107:
			case 0x000700C0: case 0x000700C1: case 0x000700C2: case 0x000700C3:
			case 0x000700C4: case 0x000700C5: case 0x000700C6: case 0x000700C7:
			case 0x000700C8: case 0x000700C9: case 0x000700CA: case 0x000700CB:
			case 0x000700CC: case 0x000700CD:
				tracked = true;
				break;
			}
		}

		if (tracked) {
			LONG n = InterlockedIncrement(&s_4D6BF0_skip);
			if (n <= 5 || (n % 100) == 0) {
				_MESSAGE("[RBRN] FIX 4D6BF0 SKIP #%ld refrFID=%08X loaded3D=%p total=%ld",
				         n, refrFID, loaded3DBefore, total);
			}
			return;  // Skip orig — preserve refr+0x3C
		}

		orig_FUN_004D6BF0(refr, NULL);
	}

	// =============================================================================
	// [RBRN] FIX v556: Block Set3D(actor, NULL) when refr+0x3C is already NULL.
	//
	// v555 caller analysis revealed THREE engine functions calling Set3D(refr, NULL)
	// ~470 times each for storm refrs in 2 minutes (~11/sec total):
	//   - FUN_0060E430 (vtable wrapper, calls Set3D after vtable[0xE0] check)
	//   - FUN_00625020 (vtable wrapper, similar pattern with vtable[0xE2])
	//   - FUN_004D9B50 (cleanup function, calls vtable[0x54](0) = Set3D(NULL))
	//
	// When Set3D is called with (NULL, NULL), it goes through the entry path:
	//     if (refr+0x3C == newModel) {
	//         if (newModel == 0) ModelLoader_CancelPendingForRefr(refr);
	//         goto exit;
	//     }
	// CancelPendingForRefr CANCELS any pending model-load tasks for the refr —
	// killing the body sub-task BEFORE it can produce. Storm refrs get this called
	// ~11 times/sec, so their body load NEVER completes.
	//
	// Fix: when called with both NULL on an actor refr (NPC/Creature), SKIP orig.
	// The function would have been a no-op + cancel; we keep the no-op semantic
	// (refr+0x3C unchanged) but skip the cancel. Pending body task can now run to
	// completion → refr+0x3C populated → TESCharacter::Update stops re-queueing →
	// storm dies naturally → no LFM UAF.
	//
	// Risk: legitimate cancellations (cell unload, refr disable) will leave stale
	// pending tasks. These should self-clean via task completion or timeout.
	// =============================================================================
	static volatile LONG s_set3d_skip_count = 0;

	static void __fastcall hook_FUN_004E0F80(void* refr, void* /*edx*/, void* newModel)
	{
		LONG total = InterlockedIncrement(&s_probe_4E0F80_total);
		void* callerRet = _ReturnAddress();

		UInt32 refrFID = 0;
		void* loaded3DBefore = NULL;
		bool tracked = false;
		bool isActor = false;

		void* actorbase = NULL;
		char fmtype = 0;

		if (refr) {
			refrFID = *(UInt32*)((char*)refr + 0x0C);
			loaded3DBefore = *(void**)((char*)refr + 0x3C);

			// Actor check: refr+0x40 = actorbase, actorbase+0x26 = formType byte.
			// 0x06 = NPC_, 0x03 = CREA.
			actorbase = *(void**)((char*)refr + 0x40);
			if (actorbase) {
				fmtype = *(char*)((char*)actorbase + 0x26);
				isActor = (fmtype == 0x06 || fmtype == 0x03);
			}

			switch (refrFID) {
			case 0x00070106:
			case 0x00070107:
			case 0x000700C0: case 0x000700C1: case 0x000700C2: case 0x000700C3:
			case 0x000700C4: case 0x000700C5: case 0x000700C6: case 0x000700C7:
			case 0x000700C8: case 0x000700C9: case 0x000700CA: case 0x000700CB:
			case 0x000700CC: case 0x000700CD:
				tracked = true;
				break;
			}
		}

		// THE FIX (v562 narrowed): skip Set3D when it would call CancelPendingForRefr,
		// but ONLY for the 16 known storm refrs (tracked list). v557's broader
		// (isActor || tracked) check broke face-load on legitimate non-tracked
		// actors because their normal Set3D-NULL→install cycle was being suppressed.
		// Tracked-only keeps the crash-suppression on storm refrs while letting
		// every other actor go through the engine's normal lifecycle.
		bool wouldCancel = (newModel == NULL) && (loaded3DBefore == NULL);
		if (tracked && wouldCancel) {
			LONG skipN = InterlockedIncrement(&s_set3d_skip_count);
			if (tracked || skipN <= 5 || (skipN % 1000) == 0) {
				const char* tag = tracked ? "[TRACKED] " : "";
				_MESSAGE("[RBRN] FIX 4E0F80 SKIP #%ld %srefrFID=%08X actorbase=%p fmtype=%02X caller=%p total=%ld",
				         skipN, tag, refrFID, actorbase, (UInt32)(UInt8)fmtype, callerRet, total);
			}
			return;  // Skip orig — no cancel, no state change
		}

		orig_FUN_004E0F80(refr, NULL, newModel);

		void* loaded3DAfter = NULL;
		if (refr) loaded3DAfter = *(void**)((char*)refr + 0x3C);

		// Log ONLY tracked when NOT skipped
		if (tracked) {
			LONG n = InterlockedIncrement(&s_probe_4E0F80_logged);
			_MESSAGE("[RBRN] PROBE 4E0F80 #%ld [TRACKED] refrFID=%08X newModel=%p "
			         "loaded3D BEFORE=%p AFTER=%p actorbase=%p fmtype=%02X caller=%p total=%ld",
			         n, refrFID, newModel, loaded3DBefore, loaded3DAfter,
			         actorbase, (UInt32)(UInt8)fmtype, callerRet, total);
		}
	}

	// =============================================================================
	// [RBRN] PROBE — FUN_0043DC00 entry+exit observer (v553 diagnostic).
	//
	// CONTEXT: v552 confirmed body NIF for storm refrs NEVER reaches FUN_0043AE10
	// nor FUN_004D7D10. Re-reading QueuedCharacter::Run revealed FUN_0043D000 only
	// handles animations for NPCs (p5=0 → body branch gate fails). The actual body
	// NIF / scene-graph load path for NPCs is elsewhere — possibly inside
	// FUN_005268D0 (called synchronously at line 11526) or via a deferred chain
	// we haven't fully traced.
	//
	// QueuedCharacter::Run (FUN_0043DC00) is the master function for NPC 3D load.
	// By hooking entry AND exit, we capture wrapper state before/after the entire
	// task. For storm refrs we'll see:
	//   - refr+0x3C: NULL on entry (expected — that's why it was queued)
	//   - wrapper+0x2c (body slot): NULL on entry (initialized to 0 by ctor)
	//   - wrapper+0x28 (face slot): may be NULL or populated
	//   - On EXIT: any of those fields populated? If wrapper+0x2c stays NULL after
	//     QueuedCharacter::Run completes, we know NO sub-task wrote to it.
	//
	// __thiscall(wrapper) with no stack args. Low risk, clean convention.
	// =============================================================================
	typedef void (__fastcall* fn_FUN_0043DC00)(void* wrapper, void* /*edx*/);
	static fn_FUN_0043DC00 orig_FUN_0043DC00 = (fn_FUN_0043DC00)0x0043DC00;
	static volatile LONG s_probe_43DC00_total = 0;
	static volatile LONG s_probe_43DC00_logged = 0;

	static void __fastcall hook_FUN_0043DC00(void* wrapper, void* /*edx*/)
	{
		LONG total = InterlockedIncrement(&s_probe_43DC00_total);

		// Read state BEFORE orig
		UInt32 refrFID = 0;
		UInt32 npcFID = 0;
		UInt32 stateBefore = 0;
		void*  refr = NULL;
		void*  loaded3DBefore = NULL;
		void*  bodySlotBefore = NULL;
		void*  faceSlotBefore = NULL;
		bool   tracked = false;

		if (wrapper) {
			stateBefore = *(UInt32*)((char*)wrapper + 0x0C);
			refr = *(void**)((char*)wrapper + 0x20);
			bodySlotBefore = *(void**)((char*)wrapper + 0x2C);
			faceSlotBefore = *(void**)((char*)wrapper + 0x28);

			if (refr) {
				refrFID = *(UInt32*)((char*)refr + 0x0C);
				loaded3DBefore = *(void**)((char*)refr + 0x3C);
				void* npc = *(void**)((char*)refr + 0x40);
				if (npc) {
					npcFID = *(UInt32*)((char*)npc + 0x0C);
				}
				switch (refrFID) {
				case 0x00070106:
				case 0x00070107:
				case 0x000700C0: case 0x000700C1: case 0x000700C2: case 0x000700C3:
				case 0x000700C4: case 0x000700C5: case 0x000700C6: case 0x000700C7:
				case 0x000700C8: case 0x000700C9: case 0x000700CA: case 0x000700CB:
				case 0x000700CC: case 0x000700CD:
					tracked = true;
					break;
				}
			}
		}

		// Call orig
		orig_FUN_0043DC00(wrapper, NULL);

		// Read state AFTER orig
		UInt32 stateAfter = 0;
		void*  loaded3DAfter = NULL;
		void*  bodySlotAfter = NULL;
		void*  faceSlotAfter = NULL;
		if (wrapper) {
			stateAfter = *(UInt32*)((char*)wrapper + 0x0C);
			bodySlotAfter = *(void**)((char*)wrapper + 0x2C);
			faceSlotAfter = *(void**)((char*)wrapper + 0x28);
			if (refr) {
				loaded3DAfter = *(void**)((char*)refr + 0x3C);
			}
		}

		bool shouldLog = tracked || (total <= 30) || (total % 200 == 0);
		if (shouldLog) {
			LONG n = InterlockedIncrement(&s_probe_43DC00_logged);
			const char* tag = tracked ? "[TRACKED] " : "";
			_MESSAGE("[RBRN] PROBE 43DC00 #%ld %srefrFID=%08X NPC=%08X "
			         "BEFORE: state=%u loaded3D=%p body=%p face=%p | "
			         "AFTER: state=%u loaded3D=%p body=%p face=%p | total=%ld",
			         n, tag, refrFID, npcFID,
			         stateBefore, loaded3DBefore, bodySlotBefore, faceSlotBefore,
			         stateAfter,  loaded3DAfter,  bodySlotAfter,  faceSlotAfter,
			         total);
		}
	}

	// =============================================================================
	// [RBRN] PROBE — FUN_0043AE10 entry observer (v552 diagnostic, RETIRED).
	//
	// CONTEXT: v551 confirmed body NIF NEVER reaches FUN_004D7D10 for storm refrs.
	// 0 [TRACKED] writes out of 2105 total. Body load fails somewhere between
	// FUN_0043BDA0 (enqueue) and FUN_004D7D10 (writer).
	//
	// FUN_0043AE10 is QueuedCharacter::vtable[13] — the immediate completion writer
	// invoked when the LFM worker delivers a produced 3D model. It calls FUN_004D7D10
	// internally to write refr+0x3C. If we see storm refrs reach FUN_0043AE10 but NOT
	// FUN_004D7D10, the link in between drops the model. If we DON'T see them at
	// FUN_0043AE10 either, the worker never produced a model for them.
	//
	// Signature: __thiscall(wrapper) with 1 stack arg (produced NiNode).
	// In: ECX = QueuedCharacter*. param_1 = produced NiNode (NULL if no model).
	// =============================================================================
	typedef void (__fastcall* fn_FUN_0043AE10)(void* wrapper, void* /*edx*/, void* produced);
	static fn_FUN_0043AE10 orig_FUN_0043AE10 = (fn_FUN_0043AE10)0x0043AE10;
	static volatile LONG s_probe_43AE10_total = 0;
	static volatile LONG s_probe_43AE10_logged = 0;

	static void __fastcall hook_FUN_0043AE10(void* wrapper, void* /*edx*/, void* produced)
	{
		LONG total = InterlockedIncrement(&s_probe_43AE10_total);

		// Read refr from wrapper+0x20
		UInt32 refrFID = 0;
		bool tracked = false;
		void* refr = NULL;
		if (wrapper) {
			refr = *(void**)((char*)wrapper + 0x20);
			if (refr) {
				refrFID = *(UInt32*)((char*)refr + 0x0C);
				switch (refrFID) {
				case 0x00070106:
				case 0x00070107:
				case 0x000700C0: case 0x000700C1: case 0x000700C2: case 0x000700C3:
				case 0x000700C4: case 0x000700C5: case 0x000700C6: case 0x000700C7:
				case 0x000700C8: case 0x000700C9: case 0x000700CA: case 0x000700CB:
				case 0x000700CC: case 0x000700CD:
					tracked = true;
					break;
				}
			}
		}

		bool nullProduced = (produced == NULL);
		bool shouldLog = tracked || nullProduced || (total <= 30) || (total % 200 == 0);
		if (shouldLog) {
			LONG n = InterlockedIncrement(&s_probe_43AE10_logged);
			const char* tag = tracked ? "[TRACKED] " : (nullProduced ? "[NULL-PROD] " : "");
			_MESSAGE("[RBRN] PROBE 43AE10 #%ld %srefrFID=%08X produced=%p wrapper=%p total=%ld",
			         n, tag, refrFID, produced, wrapper, total);
		}

		orig_FUN_0043AE10(wrapper, NULL, produced);
	}

	// =============================================================================
	// [RBRN] PROBE — FUN_0043BDA0 entry observer (v550 diagnostic, RETIRED).
	//
	// FUN_0043BDA0 is the body sub-task enqueue function. Called from FUN_0043D000
	// with the constructed Queued* sub-task struct + wrapper context. If this fires
	// for a storm refr, body load IS being attempted. If it never fires, the gating
	// in FUN_0043D000 prevented the enqueue.
	//
	// Signature: __cdecl(model_struct, ?, wrapper, path_or_null) — 4 args.
	// param_1 = body model linked-list head (or single struct)
	// param_2 = some context value
	// param_3 = the QueuedCharacter wrapper (we read refr from wrapper+0x20)
	// param_4 = optional path string (NULL for skeleton, "Data..." for SpecialAnims)
	// =============================================================================
	typedef void (__cdecl* fn_FUN_0043BDA0)(void* param_1, void* param_2, void* param_3, void* param_4);
	static fn_FUN_0043BDA0 orig_FUN_0043BDA0 = (fn_FUN_0043BDA0)0x0043BDA0;
	static volatile LONG s_probe_43BDA0_total = 0;
	static volatile LONG s_probe_43BDA0_logged = 0;

	static void __cdecl hook_FUN_0043BDA0(void* param_1, void* param_2, void* param_3, void* param_4)
	{
		LONG total = InterlockedIncrement(&s_probe_43BDA0_total);

		// Read refr from wrapper+0x20 (param_3 = wrapper)
		UInt32 refrFID = 0;
		bool tracked = false;
		void* refr = NULL;
		if (param_3) {
			refr = *(void**)((char*)param_3 + 0x20);
			if (refr) {
				refrFID = *(UInt32*)((char*)refr + 0x0C);
				switch (refrFID) {
				case 0x00070106:
				case 0x00070107:
				case 0x000700C0: case 0x000700C1: case 0x000700C2: case 0x000700C3:
				case 0x000700C4: case 0x000700C5: case 0x000700C6: case 0x000700C7:
				case 0x000700C8: case 0x000700C9: case 0x000700CA: case 0x000700CB:
				case 0x000700CC: case 0x000700CD:
					tracked = true;
					break;
				}
			}
		}

		bool hasPath = (param_4 != NULL);
		bool shouldLog = tracked || (total <= 30) || (total % 200 == 0);
		if (shouldLog) {
			LONG n = InterlockedIncrement(&s_probe_43BDA0_logged);
			const char* tag = tracked ? "[TRACKED] " : "";
			const char* pathTag = hasPath ? "[ANIM]" : "[BODY]";
			_MESSAGE("[RBRN] PROBE 43BDA0 #%ld %s%s refrFID=%08X model=%p ctx=%p wrapper=%p total=%ld",
			         n, tag, pathTag, refrFID, param_1, param_2, param_3, total);
		}

		orig_FUN_0043BDA0(param_1, param_2, param_3, param_4);
	}

	// Old probes retained as code but NOT attached in v550 (see Install() below):
	//   - hook_FUN_0043B000 (v548): retained for reference; NOT attached in v550.
	//     The body=NULL signal is now derivable from "refrFID never appears in
	//     PROBE 4D7D10 with newModel != NULL" + the storm rate.
	//   - hook_FUN_0043D000 (v549): RETIRED. It threw the timing accidentally.
	//   - hook_sub_5221C0 (v547): retained but not attached. Data was conclusive.

	// ORIGINAL v548/v549 probes below — keep code but not attached.
	// =============================================================================
	// [RBRN] PROBE — FUN_0043B000 entry observer (v548 diagnostic, NOT ATTACHED in v550).
	//
	// CONTEXT: v548's FUN_0043B000 probe confirmed the body sub-task fails to
	// produce for storm-trigger refrs. wrapper+0x2c stays NULL while wrapper+0x28
	// (face) populates correctly. ~5000 task completions per minute in DerelictMine
	// scene; ~3% have body=NULL at completion, including the storm refr 0x70107.
	//
	// FUN_0043D000 is the body-NIF dispatch function. It reads TESActorBase+0xAC
	// (the model path field), checks if path ends in "Skeleton", and conditionally
	// calls FUN_0043bda0 to enqueue the body sub-task. If the gating logic doesn't
	// call FUN_0043bda0 for our bad NPCs, body sub-task is never enqueued →
	// wrapper+0x2c stays at its ctor-zero state.
	//
	// Function signature (Ghidra): FUN_0043d000(int* param_1, undefined4 param_2,
	//   undefined4 param_3, int* param_4, char param_5, char param_6)
	// param_1 = TESActorBase pointer (or 0); model path read via vtable[0x05]
	// param_3 = wrapper (the QueuedCharacter)
	// param_4 = refr (TESObjectREFR*)
	// Calling convention: thiscall via ECX (something else); args on stack.
	//
	// Internal gates (from seg_00430000.c:10977-10987):
	//   (param_4 == NULL || vtable[0x66](0) == 0)
	//   AND ( (cVar1 == '\0' AND path_basename starts with "Skeleton") OR param_6 != 0 )
	//   AND param_5 != 0
	// → calls FUN_00435830(path, 1) + FUN_0043bda0(uVar5, ...)
	//
	// The probe captures: refr formID, NPC formID, the model path string, param_5/6,
	// and whether the strncmp("Skeleton",8)==0 succeeds.
	//
	// __thiscall (in_ECX = wrapper); explicit args pushed.
	// =============================================================================
	typedef void (__fastcall* fn_FUN_0043D000)(void* in_ECX, void* /*edx*/,
	                                           void* param_1, void* param_2, void* param_3,
	                                           void* param_4, char param_5, char param_6);
	static fn_FUN_0043D000 orig_FUN_0043D000 = (fn_FUN_0043D000)0x0043D000;
	static volatile LONG s_probe_43D000_total = 0;
	static volatile LONG s_probe_43D000_logged = 0;

	static void __fastcall hook_FUN_0043D000(void* in_ECX, void* /*edx*/,
	                                         void* param_1, void* param_2, void* param_3,
	                                         void* param_4, char param_5, char param_6)
	{
		LONG total = InterlockedIncrement(&s_probe_43D000_total);

		// Capture identifying info
		UInt32 refrFID = 0;
		UInt32 npcFID = 0;
		bool isBad = false;
		void* refr = param_4;
		if (refr && IsValidRead(refr, 0x44)) {
			refrFID = *(UInt32*)((char*)refr + 0x0C);
			void* npc = *(void**)((char*)refr + 0x40);
			if (npc && IsValidRead(npc, 0x10)) {
				npcFID = *(UInt32*)((char*)npc + 0x0C);
				isBad = IsBadActor(npc);
			}
		}

		// Read the model path. param_1 is supposed to be TESActorBase+0xAC area.
		// Engine reads via vtable[0x05] — for safety, we just dump the first 60 chars
		// of whatever string param_1 points to (treating it as a const char*).
		// Actually safer: param_1 is the TESActorBase pointer with +0xAC offset already
		// applied. So *(char**)param_1 is the model path string ptr (TESModel pattern).
		char pathBuf[64] = {0};
		bool pathValid = false;
		if (param_1 && IsValidRead(param_1, 4)) {
			char* pathPtr = *(char**)param_1;
			if (pathPtr && IsValidRead(pathPtr, 1)) {
				pathValid = true;
				for (int i = 0; i < 63; i++) {
					if (!IsValidRead(pathPtr + i, 1)) break;
					pathBuf[i] = pathPtr[i];
					if (pathBuf[i] == 0) break;
				}
				pathBuf[63] = 0;
			}
		}

		// Throttle: bad-actors always; first 30 calls; every 100th otherwise; null/empty path entries
		bool emptyPath = !pathValid || pathBuf[0] == 0;
		bool shouldLog = isBad || (total <= 30) || emptyPath || (total % 100) == 0;
		if (shouldLog) {
			LONG n = InterlockedIncrement(&s_probe_43D000_logged);
			const char* badTag = isBad ? "[BAD] " : "";
			const char* emptyTag = emptyPath ? "[EMPTY-PATH] " : "";
			_MESSAGE("[RBRN] PROBE 43D000 #%ld %s%srefr=%p refrFID=%08X NPC=%08X "
			         "p1=%p p4=%p p5=%d p6=%d path=\"%s\" total=%ld",
			         n, badTag, emptyTag, refr, refrFID, npcFID,
			         param_1, param_4, (int)param_5, (int)param_6,
			         pathBuf, total);
		}

		orig_FUN_0043D000(in_ECX, NULL, param_1, param_2, param_3, param_4, param_5, param_6);
	}

	// Old v547 probe (sub_5221C0) removed — TESRace+0x29C runtime data was empirically
	// proven identical for bad and good actors. The bug is downstream of sub_552990,
	// in the BODY sub-task path probed via FUN_0043B000 (v548) and FUN_0043D000 (v549).
	typedef void (__thiscall* fn_sub_5221C0)(void* self, void* param_1);
	static fn_sub_5221C0 orig_sub_5221C0 = (fn_sub_5221C0)0x005221C0;
	static volatile LONG s_probe_5221C0_total = 0;
	static volatile LONG s_probe_5221C0_nullslot = 0;
	static volatile LONG s_probe_5221C0_norace = 0;

	static void __fastcall hook_sub_5221C0(void* self, void* /*edx*/, void* param_1)
	{
		LONG total = InterlockedIncrement(&s_probe_5221C0_total);

		if (!self || !IsValidRead(self, 0xEC)) {
			orig_sub_5221C0(self, param_1);
			return;
		}

		UInt32 npcFormID = *(UInt32*)((char*)self + 0xC);
		void* race = *(void**)((char*)self + 0xE8);

		bool isBad = IsBadActor(self);
		const char* badTag = isBad ? "[BAD] " : "";

		if (!race) {
			LONG n = InterlockedIncrement(&s_probe_5221C0_norace);
			if (n <= 20 || (n % 200) == 0) {
				_MESSAGE("[RBRN] PROBE 5221C0 #%ld %sNPC=%08X race=NULL (fallback path) total=%ld",
				         n, badTag, npcFormID, total);
			}
			orig_sub_5221C0(self, param_1);
			return;
		}

		if (!IsValidRead(race, 0x300)) {
			orig_sub_5221C0(self, param_1);
			return;
		}

		UInt32 raceFormID = *(UInt32*)((char*)race + 0xC);
		const char* grid = (const char*)race + 0x29C;

		// 4 entries × 0x18 = 0x60 bytes total at TESRace+0x29C
		// FaceGenDataScanner finding: slots 0-2 always valid, slot 3 always v0=v4=0
		// We log slots 0-2 NULL events as the actionable signal; slot 3 is suppressed
		// since it's the engine's "padding/unused" slot.
		bool anyNull = false;
		UInt32 slotV0[4] = {0,0,0,0};
		UInt32 slotV4[4] = {0,0,0,0};
		for (int i = 0; i < 4; i++) {
			slotV0[i] = *(UInt32*)(grid + i*0x18 + 0);
			slotV4[i] = *(UInt32*)(grid + i*0x18 + 4);
			if (i < 3 && (slotV0[i] == 0 || slotV4[i] == 0)) {
				anyNull = true;
			}
		}

		// Always log first 5 calls (warm-up baseline) regardless of bad-actor status.
		bool logBaseline = (total <= 5);

		if (anyNull || logBaseline || isBad) {
			LONG n = anyNull ? InterlockedIncrement(&s_probe_5221C0_nullslot) : 0;
			const char* tag = anyNull ? "NULLPAIR" : (isBad ? "bad-baseline" : "baseline");
			// Throttle: log first 30 of each category, then every 100th
			bool shouldLog = anyNull
				? (n <= 30 || (n % 100) == 0)
				: (total <= 30 || (isBad && (total % 50) == 0));
			if (shouldLog) {
				_MESSAGE("[RBRN] PROBE 5221C0 #%ld %sNPC=%08X race=%08X %s "
				         "s0=(%08X,%08X) s1=(%08X,%08X) s2=(%08X,%08X) s3=(%08X,%08X) total=%ld",
				         n, badTag, npcFormID, raceFormID, tag,
				         slotV0[0], slotV4[0],
				         slotV0[1], slotV4[1],
				         slotV0[2], slotV4[2],
				         slotV0[3], slotV4[3],
				         total);
			}
		}

		orig_sub_5221C0(self, param_1);
	}

	static void __fastcall hook_sub_52DED0(void* self, void* /*edx*/,
	                                       void* a1, void* a2, void* a3, void* a4, void* a5)
	{
		// v562: skip for bad actors gated by kBypassBadActorLogic. The "skip → produce
		// empty B1/B2" path was the FaceGen-give-up workaround for the retry storm.
		// With v557 fixing the cancellation race upstream, the storm doesn't happen,
		// so let FaceGen run normally — that's what produces the face data we want.
		if (!kBypassBadActorLogic && IsBadActor(a3)) {
			LONG n = InterlockedIncrement(&s_moonpcSkipCount);
			if (n <= 5 || (n % 100) == 0) {
				_MESSAGE("[RBRN] sub_52DED0 skip #%ld: bad actor (TESNPC=%p formID=%08X)",
				         n, a3, *(UInt32*)((char*)a3 + 0xC));
			}
			return;
		}

		EnterCriticalSection(&s_facegenLock);
		orig_sub_52DED0(self, a1, a2, a3, a4, a5);
		LeaveCriticalSection(&s_facegenLock);
	}

	// Validate FGP critical fields before letting the engine deref them.
	// FGP layout (per BlockheadInternals.h:103-152, sizeof 0xC4):
	//   +0x70: gender flag
	//   +0x78: models.data (NiTArray internal pointer to TESModel*[])
	//   +0x88: textures.data (similar)
	//   +0x98: third array data
	//   +0xb8: eyeLeft (interior pointer into TESRace)
	//   +0xbc: eyeRight
	//
	// When any of the array data pointers are null, the engine's read at
	// 0x00555339 / similar offsets AVs. We bail out cleanly — same effect as
	// the engine's existing "skip slot" path at 0x5556de but applied at
	// function entry.
	// [RBRN] v540: removed stack-walk diagnostic (kept crashing in our SEH wrap;
	// compiler hoists deref out of __try). Full call chain already documented in
	// reference_facegen_call_chain.md. Bad actors are now in IsBadActor below.

	// =============================================================================
	// [RBRN] v538 — sub_5547F0 eyeLeft validity patch.
	//
	// Crash class observed 2026-05-05 in v537: sub_5547F0+0xAF (`mov eax, [ecx]`)
	// AVs because ecx = FGP.eyeLeft contains a CORRUPT non-NULL low-value pointer
	// (e.g. 0x000700CC = literal formID of VirtueRider, written into FGP+0xB8 by
	// some unknown engine path). The engine's existing NULL check at sub_5547F0+0x9D
	// (`cmp [eax+0xb8], 0; je skip`) catches NULL but not low-value corruption.
	//
	// Patch: replace the 12-byte cmp+je with `call check_eyeLeft + 7 NOPs`. The
	// validator checks for NULL AND for "below heap range" (< 0x01000000) values.
	// If invalid, redirect to the existing skip target at 0x00554CD4 by popping the
	// validator's return address and jumping. If valid, return normally; the NOPs
	// flow into the original `mov ecx, [eax+0xb8]` at 0x00554899.
	//
	// Original bytes at 0x0055488D:
	//   39 98 B8 00 00 00   cmp dword [eax+0xb8], ebx
	//   0F 84 3B 04 00 00   je 0x00554CD4
	// =============================================================================
	__declspec(naked) static void check_eyeLeft_validity()
	{
		__asm {
			push ecx
			mov ecx, [eax + 0xB8]    ; ecx = FGP.eyeLeft (eax = FGP from caller)
			test ecx, ecx
			jz do_skip                ; NULL → skip (matches original NULL check)
			cmp ecx, 0x01000000
			jb do_skip                ; below 16M → corrupted (formID-shaped) → skip
			pop ecx
			ret                       ; valid → continue at NOPs → original mov ecx
		do_skip:
			pop ecx
			add esp, 4                ; pop our return address
			push 0x00554CD4           ; push the original je target (eyeRight block)
			ret                       ; jump to skip path
		}
	}

	__declspec(naked) static void check_eyeRight_validity()
	{
		__asm {
			push ecx
			mov ecx, [edx + 0xBC]    ; ecx = FGP.eyeRight (edx = FGP from caller)
			test ecx, ecx
			jz do_skip                ; NULL → skip
			cmp ecx, 0x01000000
			jb do_skip                ; below 16M → corrupted → skip
			pop ecx
			ret                       ; valid → continue at NOPs → original mov ecx
		do_skip:
			pop ecx
			add esp, 4                ; pop our return address
			push 0x00555129           ; push the original je target (post-eyeRight)
			ret
		}
	}

	static void InstallPatch(UInt32 patchSite, UInt32 patchSize, void* callTarget, const char* label)
	{
		const UInt32 kRelOffset = (UInt32)callTarget - (patchSite + 5);

		UInt8 patchBytes[16];
		patchBytes[0] = 0xE8;  // call rel32
		patchBytes[1] = (UInt8)(kRelOffset & 0xFF);
		patchBytes[2] = (UInt8)((kRelOffset >> 8) & 0xFF);
		patchBytes[3] = (UInt8)((kRelOffset >> 16) & 0xFF);
		patchBytes[4] = (UInt8)((kRelOffset >> 24) & 0xFF);
		for (UInt32 i = 5; i < patchSize; i++) patchBytes[i] = 0x90;  // NOP

		DWORD oldProt = 0;
		if (!VirtualProtect((LPVOID)patchSite, patchSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
			_ERROR("[RBRN] %s patch: VirtualProtect failed (%lu)", label, GetLastError());
			return;
		}
		memcpy((void*)patchSite, patchBytes, patchSize);
		VirtualProtect((LPVOID)patchSite, patchSize, oldProt, &oldProt);
		FlushInstructionCache(GetCurrentProcess(), (LPVOID)patchSite, patchSize);
		_MESSAGE("[RBRN] %s patch installed at 0x%08X (validator at %p)", label, patchSite, callTarget);
	}

	static void InstallEyeLeftValidityPatch()
	{
		// Original bytes 0x0055488D (12): 39 98 B8 00 00 00  (cmp [eax+0xb8],ebx)
		//                                  0F 84 3B 04 00 00  (je 0x554CD4)
		InstallPatch(0x0055488D, 12, (void*)&check_eyeLeft_validity, "sub_5547F0+0x9D eyeLeft");
	}

	static void InstallEyeRightValidityPatch()
	{
		// Original bytes 0x00554CDF (12): 39 9A BC 00 00 00  (cmp [edx+0xbc],ebx)
		//                                  0F 84 3E 04 00 00  (je 0x555129)
		InstallPatch(0x00554CDF, 12, (void*)&check_eyeRight_validity, "sub_5547F0+0x4EF eyeRight");
	}

	static void __cdecl hook_DoSomething(void* faceGenNode, void* faceGenParams)
	{
		if (!faceGenParams) {
			LONG n = InterlockedIncrement(&s_doSomethingSkipCount);
			if (n <= 5 || (n % 100) == 0) {
				_MESSAGE("[RBRN] DoSomething skip #%ld: null FGP", n);
			}
			return;
		}

		char* p = (char*)faceGenParams;
		void* models   = *(void**)(p + 0x78);
		void* textures = *(void**)(p + 0x88);
		void* third    = *(void**)(p + 0x98);

		if (!models || !textures || !third) {
			LONG n = InterlockedIncrement(&s_doSomethingSkipCount);
			if (n <= 5 || (n % 100) == 0) {
				_MESSAGE("[RBRN] DoSomething skip #%ld: null array (models=%p textures=%p third=%p) FGP=%p",
				         n, models, textures, third, faceGenParams);
			}
			return;
		}

		orig_DoSomething(faceGenNode, faceGenParams);
	}

	// =============================================================================
	// [RBRN] v559 PROBES — trace upstream of the Set3D-NULL storm.
	//
	// CONTEXT: v558 stopped the LFM crash but body 3D never installs for storm refrs
	// (VirtueRider 0x70106 + horse 0x70107). Decomp tracing identified
	// TESCharacter::Update line 200 (FUN_004E0580+0x80) as the storm source:
	//
	//     if ((cellUpdateFlag) && (param_1 & 0x40000000)) {
	//         if ((flags >> 0xB & 1) == 0 && (flags >> 5 & 1) == 0) {
	//             ModelLoader_QueueReference(refr, ...)   // queue body load
	//         } else {
	//             vtable[0x54](0)   // Set3D(NULL) per frame — STORM
	//         }
	//     }
	//
	// Bit 5 (0x20) and bit 11 (0x800) are runtime "tear down 3D" state flags.
	// The patrol refrs ship with ESM flag 0x400 only (Initially Disabled / Persistent);
	// no plugin overrides them. So bits 5 & 11 are SET AT RUNTIME by something we
	// have not yet traced. Three observers answer the remaining questions:
	//
	//   438060 (ModelLoader::QueueReference)   — who is queuing body loads on
	//                                            disabled-state refrs (the actual race
	//                                            substrate)?
	//   46A9E0 (set/clear bit 5 on +0x08)      — who toggles the "disabled" bit, and
	//                                            does it stick or oscillate?
	//   46ABA0 (set/clear bit 11 on +0x08)     — same question for the VWD-shaped bit.
	//
	// All three are PURE PROBES — they call orig() unmodified. They only capture
	// _ReturnAddress() and tracked-FID flag state. Risk: log volume; mitigated by
	// per-tracked sampling (first 20 + every Nth) and tracked-only filter.
	// =============================================================================

	// --- 438060 ModelLoader::QueueReference ---------------------------------------
	// Signature (per Ghidra seg_00430000.c:6802): __thiscall on ModelLoader singleton,
	// 2 stack args (refr*, formType). Use __fastcall typedef with /*edx*/ slot to
	// absorb the unused EDX register and dispatch to __thiscall via Detours.
	typedef void (__fastcall* fn_ModelLoader_QueueReference)(void* modelLoader, void* /*edx*/, void* refr, UInt32 formType);
	static fn_ModelLoader_QueueReference orig_ModelLoader_QueueReference = (fn_ModelLoader_QueueReference)0x00438060;
	static volatile LONG s_probe_438060_total = 0;
	static volatile LONG s_probe_438060_logged = 0;

	static void __fastcall hook_ModelLoader_QueueReference(void* modelLoader, void* /*edx*/, void* refr, UInt32 formType)
	{
		LONG total = InterlockedIncrement(&s_probe_438060_total);
		void* callerRet = _ReturnAddress();

		UInt32 refrFID = 0;
		UInt32 refrFlags = 0;
		void* loaded3D = NULL;
		bool tracked = false;

		if (refr) {
			refrFID    = *(UInt32*)((char*)refr + 0x0C);
			refrFlags  = *(UInt32*)((char*)refr + 0x08);
			loaded3D   = *(void**)((char*)refr + 0x3C);
			switch (refrFID) {
			case 0x00070106: case 0x00070107:
			case 0x000700C0: case 0x000700C1: case 0x000700C2: case 0x000700C3:
			case 0x000700C4: case 0x000700C5: case 0x000700C6: case 0x000700C7:
			case 0x000700C8: case 0x000700C9: case 0x000700CA: case 0x000700CB:
			case 0x000700CC: case 0x000700CD:
				tracked = true;
				break;
			}
		}

		if (tracked) {
			LONG n = InterlockedIncrement(&s_probe_438060_logged);
			if (n <= 20 || (n % 50) == 0) {
				_MESSAGE("[RBRN] PROBE 438060 #%ld [TRACKED] refrFID=%08X flags=%08X (bit5=%lu bit11=%lu) loaded3D=%p formType=%u caller=%p total=%ld",
				         n, refrFID, refrFlags,
				         (unsigned long)((refrFlags >> 5) & 1), (unsigned long)((refrFlags >> 11) & 1),
				         loaded3D, formType, callerRet, total);
			}
		}

		orig_ModelLoader_QueueReference(modelLoader, NULL, refr, formType);
	}

	// --- 46A9E0  set/clear bit 5 (0x20) on this+0x08 -----------------------------
	// Signature (per Ghidra seg_00460000.c:8056): __thiscall, 1 stack arg (char param).
	// Body: if param==0 clear bit 5, else set bit 5; then dispatch vtable[0x10] propagator.
	typedef void (__fastcall* fn_FUN_0046A9E0)(void* self, void* /*edx*/, char param);
	static fn_FUN_0046A9E0 orig_FUN_0046A9E0 = (fn_FUN_0046A9E0)0x0046A9E0;
	static volatile LONG s_probe_46A9E0_total = 0;
	static volatile LONG s_probe_46A9E0_logged = 0;

	static void __fastcall hook_FUN_0046A9E0(void* self, void* /*edx*/, char param)
	{
		LONG total = InterlockedIncrement(&s_probe_46A9E0_total);
		void* callerRet = _ReturnAddress();

		UInt32 fid = 0;
		UInt32 flagsBefore = 0;
		bool tracked = false;

		if (self) {
			fid         = *(UInt32*)((char*)self + 0x0C);
			flagsBefore = *(UInt32*)((char*)self + 0x08);
			switch (fid) {
			case 0x00070106: case 0x00070107:
			case 0x000700C0: case 0x000700C1: case 0x000700C2: case 0x000700C3:
			case 0x000700C4: case 0x000700C5: case 0x000700C6: case 0x000700C7:
			case 0x000700C8: case 0x000700C9: case 0x000700CA: case 0x000700CB:
			case 0x000700CC: case 0x000700CD:
				tracked = true;
				break;
			}
		}

		if (tracked) {
			LONG n = InterlockedIncrement(&s_probe_46A9E0_logged);
			if (n <= 20 || (n % 200) == 0) {
				_MESSAGE("[RBRN] PROBE 46A9E0 #%ld [TRACKED] fid=%08X flagsBefore=%08X bit5Before=%lu param=%d willSetBit5=%d caller=%p total=%ld",
				         n, fid, flagsBefore,
				         (unsigned long)((flagsBefore >> 5) & 1),
				         (int)param, (param != 0) ? 1 : 0,
				         callerRet, total);
			}
		}

		orig_FUN_0046A9E0(self, NULL, param);
	}

	// --- 46ABA0  set/clear bit 11 (0x800) on this+0x08 ---------------------------
	// Signature (per Ghidra seg_00460000.c:8162): same shape as FUN_0046A9E0.
	typedef void (__fastcall* fn_FUN_0046ABA0)(void* self, void* /*edx*/, char param);
	static fn_FUN_0046ABA0 orig_FUN_0046ABA0 = (fn_FUN_0046ABA0)0x0046ABA0;
	static volatile LONG s_probe_46ABA0_total = 0;
	static volatile LONG s_probe_46ABA0_logged = 0;

	static void __fastcall hook_FUN_0046ABA0(void* self, void* /*edx*/, char param)
	{
		LONG total = InterlockedIncrement(&s_probe_46ABA0_total);
		void* callerRet = _ReturnAddress();

		UInt32 fid = 0;
		UInt32 flagsBefore = 0;
		bool tracked = false;

		if (self) {
			fid         = *(UInt32*)((char*)self + 0x0C);
			flagsBefore = *(UInt32*)((char*)self + 0x08);
			switch (fid) {
			case 0x00070106: case 0x00070107:
			case 0x000700C0: case 0x000700C1: case 0x000700C2: case 0x000700C3:
			case 0x000700C4: case 0x000700C5: case 0x000700C6: case 0x000700C7:
			case 0x000700C8: case 0x000700C9: case 0x000700CA: case 0x000700CB:
			case 0x000700CC: case 0x000700CD:
				tracked = true;
				break;
			}
		}

		if (tracked) {
			LONG n = InterlockedIncrement(&s_probe_46ABA0_logged);
			if (n <= 20 || (n % 200) == 0) {
				_MESSAGE("[RBRN] PROBE 46ABA0 #%ld [TRACKED] fid=%08X flagsBefore=%08X bit11Before=%lu param=%d willSetBit11=%d caller=%p total=%ld",
				         n, fid, flagsBefore,
				         (unsigned long)((flagsBefore >> 11) & 1),
				         (int)param, (param != 0) ? 1 : 0,
				         callerRet, total);
			}
		}

		orig_FUN_0046ABA0(self, NULL, param);
	}

	// =============================================================================
	// [V565] INSTRUMENTATION — capture helper.eyeLeft (FGP+0xB8) progression
	// through the FaceGen pipeline to identify the corruption window.
	//
	// Pipeline (per reference_facegen_call_chain.md + decompile of these fns):
	//   sub_52CD50(race, npc, helper)       writes helper+0xB8 = race+0x188 (canon)
	//   sub_555A80(out1, out2, helper, fl)  reads helper+0xB8; if non-zero & non-zero
	//                                        eyeRight, calls sub_5547F0
	//   sub_5547F0(out1, out2, helper, fl)  derefs helper+0xB8 → AV when corrupt
	//                                        (vtable[5] call at sub_5547F0+0xB6)
	//
	// We hook all 3 and snapshot helper+0xB8 at each. Logs interleave so we can see
	// where the value changes:
	//   - Same canon at all 3 → corruption inside sub_5547F0 itself
	//   - Differs between 52CD50 EXIT and 555A80 ENTRY → corruption in between
	//   - Differs between 555A80 ENTRY and 5547F0 ENTRY → corruption inside 555A80
	//
	// Filter: only log when NPC is in IsBadActor blacklist.
	// Helper-address → NPC mapping via small CS-protected ring buffer (NO TLS).
	// Pointer reads validated via VirtualQuery (IsValidRead).
	// =============================================================================

	typedef void (__fastcall* fn_sub_52CD50)(void* race, void* /*edx*/, void* npc, void* helper);
	static fn_sub_52CD50 orig_sub_52CD50 = (fn_sub_52CD50)0x0052CD50;

	typedef void (__cdecl* fn_sub_555A80)(void* out1, void* out2, void* helper, char flag);
	static fn_sub_555A80 orig_sub_555A80 = (fn_sub_555A80)0x00555A80;

	typedef void (__cdecl* fn_sub_5547F0)(void* out1, void* out2, void* helper, char flag);
	static fn_sub_5547F0 orig_sub_5547F0 = (fn_sub_5547F0)0x005547F0;

	// v568 — confirm whether sub_52DED0 (the worker-chain chokepoint) is ever invoked.
	// If it never fires, the crash isn't in that chain — it's downstream of Blockhead's
	// SwapFaceGenHeadData on the synchronous TESRace::GetFaceGenHeadParameters path.
	typedef void (__fastcall* fn_v568_sub_52DED0)(void* race, void* /*edx*/, void* a1, void* a2, void* npc, void* a4, void* a5);
	static fn_v568_sub_52DED0 orig_v568_sub_52DED0 = (fn_v568_sub_52DED0)0x0052DED0;
	static volatile LONG s_v568_log_52DED0 = 0;

	struct V565Context {
		void*  helper;
		UInt32 npcFID;
		UInt32 raceFID;
		UInt32 expectedEyeLeft;   // what sub_52CD50 set it to (race+0x188)
		UInt32 expectedEyeRight;  // race+0x1A0
		DWORD  threadId;
	};
	static V565Context  s_v568_ctx[16];
	static CRITICAL_SECTION s_v568_lock;
	static bool         s_v568_lockInit = false;
	static volatile LONG s_v568_ctxIdx = 0;
	static volatile LONG s_v568_log_52CD50 = 0;
	static volatile LONG s_v568_log_555A80 = 0;
	static volatile LONG s_v568_log_5547F0 = 0;

	static void V565RecordContext(void* helper, UInt32 npcFID, UInt32 raceFID,
	                              UInt32 expL, UInt32 expR)
	{
		LONG idx = InterlockedIncrement(&s_v568_ctxIdx) % 16;
		EnterCriticalSection(&s_v568_lock);
		s_v568_ctx[idx].helper = helper;
		s_v568_ctx[idx].npcFID = npcFID;
		s_v568_ctx[idx].raceFID = raceFID;
		s_v568_ctx[idx].expectedEyeLeft = expL;
		s_v568_ctx[idx].expectedEyeRight = expR;
		s_v568_ctx[idx].threadId = GetCurrentThreadId();
		LeaveCriticalSection(&s_v568_lock);
	}

	// Lookup most-recent context for a helper (scan all entries, return latest match).
	// Returns true if found.
	static bool V565LookupContext(void* helper, V565Context* out)
	{
		EnterCriticalSection(&s_v568_lock);
		bool found = false;
		// Scan in reverse from most-recent; tie-break on threadId.
		DWORD tid = GetCurrentThreadId();
		// First pass: same-thread match
		for (int i = 0; i < 16; i++) {
			if (s_v568_ctx[i].helper == helper && s_v568_ctx[i].threadId == tid) {
				if (out) *out = s_v568_ctx[i];
				found = true;
				break;
			}
		}
		// Second pass: any-thread match
		if (!found) {
			for (int i = 0; i < 16; i++) {
				if (s_v568_ctx[i].helper == helper) {
					if (out) *out = s_v568_ctx[i];
					found = true;
					break;
				}
			}
		}
		LeaveCriticalSection(&s_v568_lock);
		return found;
	}

	static bool V565IsBadActorByFID(UInt32 fullFID)
	{
		UInt32 local = fullFID & 0x00FFFFFF;
		for (UInt32 i = 0; i < kBadActorCount; i++) {
			if (local == kBadActorLocalFormIDs[i]) return true;
		}
		return false;
	}

	static void __fastcall hook_v568_sub_52CD50(void* race, void* /*edx*/, void* npc, void* helper)
	{
		UInt32 npcFID = 0;
		UInt32 raceFID = 0;
		bool isBad = false;

		if (npc && IsValidRead((const char*)npc + 0x0C, 4)) {
			npcFID = *(const UInt32*)((const char*)npc + 0x0C);
			isBad = V565IsBadActorByFID(npcFID);
		}
		if (race && IsValidRead((const char*)race + 0x0C, 4)) {
			raceFID = *(const UInt32*)((const char*)race + 0x0C);
		}

		UInt32 expL = race ? ((UInt32)race + 0x188) : 0;
		UInt32 expR = race ? ((UInt32)race + 0x1A0) : 0;

		// Always record (so downstream lookups work for everyone), log only bad actors.
		if (helper) {
			V565RecordContext(helper, npcFID, raceFID, expL, expR);
		}

		// Run original.
		orig_sub_52CD50(race, NULL, npc, helper);

		// Snapshot eyeLeft AFTER sub_52CD50 wrote it.
		if (isBad && helper && IsValidRead((const char*)helper + 0xBC, 4)) {
			UInt32 eyeL = *(const UInt32*)((const char*)helper + 0xB8);
			UInt32 eyeR = *(const UInt32*)((const char*)helper + 0xBC);
			LONG n = InterlockedIncrement(&s_v568_log_52CD50);
			if (n <= 30 || (n % 100) == 0) {
				const char* tag = (eyeL == expL) ? "OK" : "MISMATCH";
				_MESSAGE("[V565] 52CD50 EXIT #%ld npc=%08X race=%08X helper=%p eyeL=%08X(exp=%08X) eyeR=%08X(exp=%08X) %s tid=%lu",
				         n, npcFID, raceFID, helper, eyeL, expL, eyeR, expR, tag,
				         GetCurrentThreadId());
			}
		}
	}

	static void __cdecl hook_v568_sub_555A80(void* out1, void* out2, void* helper, char flag)
	{
		LONG n = InterlockedIncrement(&s_v568_log_555A80);

		// Unconditional first-N log — proves the hook fires even when our filter rejects.
		if (n <= 30) {
			UInt32 eyeL = 0, eyeR = 0;
			bool readable = (helper && IsValidRead((const char*)helper + 0xBC, 4));
			if (readable) {
				eyeL = *(const UInt32*)((const char*)helper + 0xB8);
				eyeR = *(const UInt32*)((const char*)helper + 0xBC);
			}
			V565Context ctx; ZeroMemory(&ctx, sizeof(ctx));
			bool gotCtx = V565LookupContext(helper, &ctx);
			bool isBad = gotCtx && V565IsBadActorByFID(ctx.npcFID);
			_MESSAGE("[V565] 555A80 ENTRY #%ld helper=%p readable=%d eyeL=%08X eyeR=%08X gotCtx=%d npc=%08X canonExpL=%08X bad=%d tid=%lu",
			         n, helper, readable ? 1 : 0, eyeL, eyeR,
			         gotCtx ? 1 : 0, ctx.npcFID, ctx.expectedEyeLeft,
			         isBad ? 1 : 0, GetCurrentThreadId());
		}
		orig_sub_555A80(out1, out2, helper, flag);
	}

	static void __fastcall hook_v568_sub_52DED0(void* race, void* /*edx*/, void* a1, void* a2, void* npc, void* a4, void* a5)
	{
		LONG n = InterlockedIncrement(&s_v568_log_52DED0);
		if (n <= 30) {
			UInt32 npcFID = 0, raceFID = 0;
			if (npc && IsValidRead((const char*)npc + 0x0C, 4)) {
				npcFID = *(const UInt32*)((const char*)npc + 0x0C);
			}
			if (race && IsValidRead((const char*)race + 0x0C, 4)) {
				raceFID = *(const UInt32*)((const char*)race + 0x0C);
			}
			_MESSAGE("[V566] 52DED0 ENTRY #%ld race=%p(%08X) npc=%p(%08X) a1=%p a2=%p a4=%p a5=%p tid=%lu",
			         n, race, raceFID, npc, npcFID, a1, a2, a4, a5, GetCurrentThreadId());
		}
		orig_v568_sub_52DED0(race, NULL, a1, a2, npc, a4, a5);
	}

	static void __cdecl hook_v568_sub_5547F0(void* out1, void* out2, void* helper, char flag)
	{
		LONG n = InterlockedIncrement(&s_v568_log_5547F0);

		// Unconditional first-N log to prove hook fires.
		if (n <= 30) {
			UInt32 eyeL = 0, eyeR = 0, vtableAtEyeL = 0;
			bool readable = (helper && IsValidRead((const char*)helper + 0xBC, 4));
			bool eyeLReadable = false;
			if (readable) {
				eyeL = *(const UInt32*)((const char*)helper + 0xB8);
				eyeR = *(const UInt32*)((const char*)helper + 0xBC);
				if (eyeL && IsValidRead((const void*)eyeL, 4)) {
					vtableAtEyeL = *(const UInt32*)eyeL;
					eyeLReadable = true;
				}
			}
			V565Context ctx; ZeroMemory(&ctx, sizeof(ctx));
			bool gotCtx = V565LookupContext(helper, &ctx);
			_MESSAGE("[V565] 5547F0 ENTRY #%ld helper=%p readable=%d eyeL=%08X(canon=%08X) eyeR=%08X [eyeL]=%08X eyeLReadable=%d gotCtx=%d npc=%08X tid=%lu",
			         n, helper, readable ? 1 : 0, eyeL, ctx.expectedEyeLeft, eyeR,
			         vtableAtEyeL, eyeLReadable ? 1 : 0, gotCtx ? 1 : 0, ctx.npcFID,
			         GetCurrentThreadId());
		}
		orig_sub_5547F0(out1, out2, helper, flag);
	}

	// =============================================================================
	// [V568] Storm-refr body-load investigation.
	//
	// CONTEXT (post-v568): Two patrol refrs `0x70106` (VirtueRider) + `0x70107`
	// (his horse) are stable (no crash thanks to v557+v558) but body never installs.
	// Per END_TO_END_v2 trace and v565/v566 probe data, TESCharacter::Update
	// (FUN_004E0580 line 200) has a per-frame check:
	//
	//     if (refr.flags & (1<<5 | 1<<11)) → vtable[0x54](0)         // tear-down
	//     else if (refr+0x3C == 0 && refr+0x40 != 0 && fmtype is actor)
	//         ModelLoader::QueueReference(refr, ...)                   // queue load
	//
	// For the storm refrs, runtime flags have bit 5 OR bit 11 set, so engine
	// always takes the tear-down branch and never queues body load. v559 probes
	// confirmed: zero QueueReference events for tracked refrs across an entire
	// session. v559 also confirmed the official bit-5/bit-11 setters
	// (FUN_0046A9E0 / FUN_0046ABA0) NEVER fire for tracked refrs — meaning the
	// bits are set by some path we haven't traced (direct memory write, or set
	// at cell-attach before our hooks attach).
	//
	// v568 strategy: hook TESCharacter::Update entry, observe flags state, and
	// force-clear bits 5/11 for tracked storm refrs BEFORE orig runs. If body
	// load now queues and the refr renders, we have a working empirical fix
	// (with the caveat that we don't fully understand the bit semantics —
	// they might gate other behavior we haven't observed yet).
	// =============================================================================

	typedef void (__fastcall* fn_TESCharacterUpdate)(void* refr, void* /*edx*/, UInt32 param);
	static fn_TESCharacterUpdate orig_TESCharacterUpdate = (fn_TESCharacterUpdate)0x004E0580;
	static volatile LONG s_v568_log_count = 0;
	static volatile LONG s_v568_clear_count = 0;

	// Set false to observe-only without modifying flags.
	static const bool kV568ForceClear = true;

	static void __fastcall hook_v568_TESCharacterUpdate(void* refr, void* /*edx*/, UInt32 param)
	{
		if (refr) {
			UInt32 refrFID = *(UInt32*)((char*)refr + 0x0C);
			bool tracked = (refrFID == 0x00070106) || (refrFID == 0x00070107);
			if (tracked) {
				UInt32 flagsBefore = *(UInt32*)((char*)refr + 0x08);
				int bit5  = (flagsBefore >> 5) & 1;
				int bit11 = (flagsBefore >> 11) & 1;

				LONG n = InterlockedIncrement(&s_v568_log_count);

				if (kV568ForceClear && (bit5 || bit11)) {
					UInt32 flagsAfter = flagsBefore & ~((1U << 5) | (1U << 11));
					*(UInt32*)((char*)refr + 0x08) = flagsAfter;
					LONG c = InterlockedIncrement(&s_v568_clear_count);
					if (c <= 30 || (c % 200) == 0) {
						_MESSAGE("[V568] TESCharUpdate #%ld CLEAR fid=%08X param=%08X before=%08X(b5=%d b11=%d) after=%08X tid=%lu",
						         c, refrFID, param, flagsBefore, bit5, bit11, flagsAfter,
						         GetCurrentThreadId());
					}
				}
				else if (n <= 30 || (n % 500) == 0) {
					_MESSAGE("[V568] TESCharUpdate #%ld OBSERVE fid=%08X param=%08X flags=%08X(b5=%d b11=%d) tid=%lu",
					         n, refrFID, param, flagsBefore, bit5, bit11,
					         GetCurrentThreadId());
				}
			}
		}
		orig_TESCharacterUpdate(refr, NULL, param);
	}

	bool Install()
	{
		if (s_installed) return true;

		// v562 strategy: keep every layer that fixed an OBSERVED crash signature,
		// drop only Layer 6 (the bad-actor blacklist hooks that install a sentinel
		// face). Layer 6 was a "give up on FaceGen for these NPCs" workaround for
		// the cancellation-cycle UAF; v557 fixes that race upstream, so the
		// give-up isn't needed and was the thing hiding the face.
		//
		//   KEEP: Layer 1 (LFM NOPs), Layer 2 (sub_52DED0 mutex),
		//         Layer 3 (AgeMorphTable redirect), Layer 4 (DoSomething FGP
		//         validator), Layer 5 (eye binary patches),
		//         v557 (Set3D-NULL skip — narrowed to tracked-only),
		//         v558 (FUN_004D6BF0 skip — already tracked-only)
		//   DROP: Layer 6 hooks (sub_4348B0/sub_435300/sub_522260/sub_528D90),
		//         InitSentinel(), v559 probes (diagnostic-only)

		// Layer 1: LFM NOPs — patches the deferred-free chain, prevents UAF.
		_MemHdlr(BucketArrayFreeChainA).WriteNop();
		_MemHdlr(BucketArrayFreeChainB).WriteNop();
		_MESSAGE("[RBRN] EngineRaceFix v568: LFM bucket-array FormHeapFree NOPs applied");

		// Layer 3: AgeMorphTable validation-failure redirect.
		WriteRelJump(0x006EDDD4, 0x006EDD8F);
		_MESSAGE("[RBRN] EngineRaceFix: AgeMorphTable validation-failure redirect installed (0x006EDDD4 -> 0x006EDD8F)");

		// Layer 5: sub_5547F0 eyeLeft + eyeRight validity patches.
		InstallEyeLeftValidityPatch();
		InstallEyeRightValidityPatch();

		// Layer 2 + Layer 4 + v557 + v558 hooks. Layer 6 (sub_435300, sub_522260,
		// sub_528D90, sub_4348B0) and v559 probes (438060, 46A9E0, 46ABA0) are
		// intentionally NOT attached.
		InitializeCriticalSectionAndSpinCount(&s_facegenLock, 4000);

		LONG err = DetourTransactionBegin();
		if (err != NO_ERROR) {
			_ERROR("[RBRN] EngineRaceFix: DetourTransactionBegin failed (%ld)", err);
			DeleteCriticalSection(&s_facegenLock);
			return false;
		}
		DetourUpdateThread(GetCurrentThread());

		// v568 INSTRUMENTATION: attach 3 probe hooks to capture eyeLeft progression.
		// v569 PRODUCTION: only attach proven crash-prevention hooks.
		// All v559/v565/v566/v568 diagnostic probes stripped.
		// Layer 6 (bad-actor blacklist sentinel-install) hooks stay defined for
		// reference but are NOT attached — v567 fixes the actual crash upstream.
		err |= DetourAttach(&(PVOID&)orig_DoSomething,    hook_DoSomething);    // Layer 4: NULL FGP-array validator
		err |= DetourAttach(&(PVOID&)orig_FUN_004E0F80,   hook_FUN_004E0F80);   // v557: Set3D-NULL skip for tracked storm refrs
		err |= DetourAttach(&(PVOID&)orig_FUN_004D6BF0,   hook_FUN_004D6BF0);   // v558: FUN_004D6BF0 direct-clear skip for tracked

		if (err != NO_ERROR) {
			_ERROR("[RBRN] EngineRaceFix: DetourAttach failed (%ld)", err);
			DetourTransactionAbort();
			DeleteCriticalSection(&s_facegenLock);
			return false;
		}

		err = DetourTransactionCommit();
		if (err != NO_ERROR) {
			_ERROR("[RBRN] EngineRaceFix: DetourTransactionCommit failed (%ld)", err);
			DeleteCriticalSection(&s_facegenLock);
			return false;
		}

		s_installed = true;
		_MESSAGE("[RBRN] EngineRaceFix v581 PRODUCTION: re-applies v570's 50ms time-based reset on thread_local retry-loop dedup state. Hotel head-loss fix the user originally validated on save loads (the earlier 'new game crashes' was PSMQD/LINK.esp UI null-deref, since fixed). No Detach3D modifications.");
		return true;
	}

	void Uninstall()
	{
		if (!s_installed) return;

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourDetach(&(PVOID&)orig_DoSomething,  hook_DoSomething);
		DetourDetach(&(PVOID&)orig_FUN_004E0F80, hook_FUN_004E0F80);
		DetourDetach(&(PVOID&)orig_FUN_004D6BF0, hook_FUN_004D6BF0);
		DetourTransactionCommit();
		DeleteCriticalSection(&s_facegenLock);

		s_installed = false;
	}
}
