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
		DetourTransactionCommit();
		DeleteCriticalSection(&s_facegenLock);

		s_installed = false;
	}
}
