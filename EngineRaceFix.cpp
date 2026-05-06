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
		if (IsBadActor(self)) {
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
		if (IsBadActor(self)) {
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

		if (!IsBadActor(npc)) {
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

		// THE FIX (v557 broadened): skip Set3D when it would call CancelPendingForRefr.
		// Trigger condition: (isActor OR tracked refrFID) + both refr+0x3C and newModel NULL.
		// v556's isActor-only check fired 0 times, suggesting refr.actorbase is NULL
		// or has unexpected formType when Set3D is called. The tracked-refrFID fallback
		// guarantees the fix at least covers known storm refrs.
		bool wouldCancel = (newModel == NULL) && (loaded3DBefore == NULL);
		if ((isActor || tracked) && wouldCancel) {
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
		// Skip known bad actors: empty B1/B2 produces a DoSomething AV which Layer 4
		// catches but the retry storm exhausts the LFM hazard-pointer protocol → UAF.
		// Skipping here eliminates the trigger entirely (no allocation, no churn).
		if (IsBadActor(a3)) {
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

	bool Install()
	{
		if (s_installed) return true;

		// Layer 1: LFM NOPs.
		_MemHdlr(BucketArrayFreeChainA).WriteNop();
		_MemHdlr(BucketArrayFreeChainB).WriteNop();
		_MESSAGE("[RBRN] EngineRaceFix: LFM bucket-array FormHeapFree NOPs applied");

		// Layer 3: AgeMorphTable validation-failure redirect (0x006EDDD4 -> 0x006EDD8F).
		WriteRelJump(0x006EDDD4, 0x006EDD8F);
		_MESSAGE("[RBRN] EngineRaceFix: AgeMorphTable validation-failure redirect installed (0x006EDDD4 -> 0x006EDD8F)");

		// Layer 5 (v538+): sub_5547F0 eyeLeft + eyeRight validity patches.
		// Catches FGP+0xB8/0xBC corruption where the engine writes a low-value
		// (formID-shaped) value into the slot, defeating the existing NULL check.
		InstallEyeLeftValidityPatch();
		InstallEyeRightValidityPatch();

		// Layer 6 (v541): BSFaceGenNiNode sentinel for storm-stop.
		// Hook QueuedHead::Run; for bad actors install sentinel face nodes so the
		// engine stops re-queueing → eliminates LFM hazard-pointer protocol stress.
		InitSentinel();

		// Layers 2 + 4: Detours hooks.
		InitializeCriticalSectionAndSpinCount(&s_facegenLock, 4000);

		LONG err = DetourTransactionBegin();
		if (err != NO_ERROR) {
			_ERROR("[RBRN] EngineRaceFix: DetourTransactionBegin failed (%ld)", err);
			DeleteCriticalSection(&s_facegenLock);
			return false;
		}
		DetourUpdateThread(GetCurrentThread());

		err |= DetourAttach(&(PVOID&)orig_sub_52DED0,  hook_sub_52DED0);
		err |= DetourAttach(&(PVOID&)orig_sub_435300,  hook_sub_435300);
		err |= DetourAttach(&(PVOID&)orig_sub_522260,  hook_sub_522260);
		err |= DetourAttach(&(PVOID&)orig_sub_528D90,  hook_sub_528D90);
		err |= DetourAttach(&(PVOID&)orig_sub_4348B0,  hook_sub_4348B0);
		err |= DetourAttach(&(PVOID&)orig_DoSomething, hook_DoSomething);
		// v547 (sub_5221C0), v548 (FUN_0043B000), v549 (FUN_0043D000) probes RETIRED.
		// v549's FUN_0043D000 hook accidentally throttled the storm via probe latency.
		// v550 probes target the canonical refr+0x3C writer + body sub-task enqueue.
		err |= DetourAttach(&(PVOID&)orig_FUN_004D7D10, hook_FUN_004D7D10);  // [RBRN] v550 probe — canonical refr+0x3C writer
		err |= DetourAttach(&(PVOID&)orig_FUN_004E0F80, hook_FUN_004E0F80);  // [RBRN] v554/557 probe + FIX — Set3D
		err |= DetourAttach(&(PVOID&)orig_FUN_004D6BF0, hook_FUN_004D6BF0);  // [RBRN] v558 FIX — direct Detach3D clearer

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
		_MESSAGE("[RBRN] EngineRaceFix: stack ready (LFM NOPs + sub_52DED0 mutex + AgeMorphTable redirect + DoSomething FGP validator)");
		return true;
	}

	void Uninstall()
	{
		if (!s_installed) return;

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourDetach(&(PVOID&)orig_sub_52DED0,  hook_sub_52DED0);
		DetourDetach(&(PVOID&)orig_sub_435300,  hook_sub_435300);
		DetourDetach(&(PVOID&)orig_sub_522260,  hook_sub_522260);
		DetourDetach(&(PVOID&)orig_sub_528D90,  hook_sub_528D90);
		DetourDetach(&(PVOID&)orig_sub_4348B0,  hook_sub_4348B0);
		DetourDetach(&(PVOID&)orig_DoSomething, hook_DoSomething);
		DetourDetach(&(PVOID&)orig_FUN_004D7D10, hook_FUN_004D7D10);  // [RBRN] v550 probe
		DetourDetach(&(PVOID&)orig_FUN_004E0F80, hook_FUN_004E0F80);  // [RBRN] v554/557
		DetourDetach(&(PVOID&)orig_FUN_004D6BF0, hook_FUN_004D6BF0);  // [RBRN] v558
		DetourTransactionCommit();
		DeleteCriticalSection(&s_facegenLock);

		s_installed = false;
	}
}
