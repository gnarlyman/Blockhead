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
	// Three layers, each addressing a distinct manifestation of the same underlying
	// condition: BSFaceGen worker thread receives an FGP-shaped struct whose
	// NiTArray data pointers are NULL. v513-v517 strip-and-trace investigation
	// (2026-05-04) confirmed the storm originates from sub_52DED0 (called from
	// sub_5227A0 with hardcoded flag=1 at 0x005232A9). The "corrupt FGP" is
	// actually a DIFFERENT struct (0x1E0 / 0x118 bytes — real FGP is 0xC4) that
	// happens to have NiTArray-shaped fields at +0x78 / +0x88 / +0x98. The
	// arrays are constructed-but-empty (vtable set, data=NULL) which is normal
	// post-construction state. Layer 4 catches the precondition violation when
	// DoSomething is called on these unpopulated objects. See
	// feedback_facegen_storm_root_cause.md.
	//
	//   Layer 2. sub_52DED0 worker face-load chokepoint mutex — wraps the
	//      worker chain (BSTaskThread_Runnable -> sub_523220 -> sub_9F88B0 ->
	//      sub_5547F0) so it can't read FGP slots while the main thread is
	//      mid-mutation. Without this, the worker AVs at sub_5547F0+0x2E9.
	//
	//   Layer 3. AgeMorphTable validation-failure redirect — replaces call to
	//      _invalid_parameter at 0x006EDDD4 with jmp to existing "return 0.0"
	//      early-exit path at 0x006EDD8F. Independent CRT-fail-fast bug class
	//      that fires from any code path hitting AgeMorphTable::Lookup with an
	//      empty morph vector. 5-byte binary patch.
	//
	//   Layer 4. BSFaceGen_DoSomething FGP-validation hook — validates the
	//      FGP's models.data / textures.data / third array pointers (offsets
	//      0x78, 0x88, 0x98) at function entry; bails cleanly if any are null.
	//      Primary defense; fires ~2500 times per session in mounted-patrol
	//      streams.
	//
	// Removed in v515 strip-test (no regression observed): LFM bucket-array
	// NOPs at 0x43296E and 0x4327EC, per-FGP g_FGPLocks lock-map.
	// =============================================================================

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

	static void __cdecl hook_DoSomething(void* faceGenNode, void* faceGenParams)
	{
		if (!faceGenParams) {
			LONG n = InterlockedIncrement(&s_doSomethingSkipCount);
			if (n <= 5 || (n % 1000) == 0) {
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
			if (n <= 5 || (n % 1000) == 0) {
				_MESSAGE("[RBRN] DoSomething skip #%ld: null array FGP=%p", n, faceGenParams);
			}
			return;
		}

		orig_DoSomething(faceGenNode, faceGenParams);
	}

	bool Install()
	{
		if (s_installed) return true;

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
		_MESSAGE("[RBRN] EngineRaceFix: stack ready (sub_52DED0 mutex + AgeMorphTable redirect + DoSomething FGP validator)");
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
