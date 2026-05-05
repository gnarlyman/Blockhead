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

	// BSFaceGen_DoSomethingWithFaceGenNode FGP-validation hook.
	typedef void (__cdecl* fn_DoSomething)(void* faceGenNode, void* faceGenParams);
	static fn_DoSomething orig_DoSomething = (fn_DoSomething)0x005551C0;

	static CRITICAL_SECTION s_facegenLock;
	static bool             s_installed = false;

	static volatile LONG    s_doSomethingSkipCount = 0;

	static void __fastcall hook_sub_52DED0(void* self, void* /*edx*/,
	                                       void* a1, void* a2, void* a3, void* a4, void* a5)
	{
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
		DetourDetach(&(PVOID&)orig_DoSomething, hook_DoSomething);
		DetourTransactionCommit();
		DeleteCriticalSection(&s_facegenLock);

		s_installed = false;
	}
}
