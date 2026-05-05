#include "EngineRaceFix.h"
#include "BlockheadInternals.h"

#include <windows.h>
#include <intrin.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "Detours/detours.h"

#pragma comment(lib, "Detours/detours.lib")

namespace EngineRaceFix
{
	// =============================================================================
	// [RBRN] BSFaceGen FGP-corruption-tolerant fix stack (post-strip + relayer).
	//
	// 2026-05-04 audit removed four "defense-in-depth" layers in v513. Strip
	// test confirmed three were truly redundant (LFM bucket-array NOPs, per-FGP
	// lock map, retry-loop guard — and we put back retry-loop guard for perf
	// reasons in v514). v514 then crashed on the worker face-load chain at
	// sub_5547F0+0x2E9, confirming the sub_52DED0 chokepoint mutex was load-
	// bearing for crash prevention (not just symptom-level): it serialized
	// BSTaskManagerThread reads of FGP against main-thread FGP mutation. So
	// it's back in v515.
	//
	//   Layer 2. sub_52DED0 worker face-load chokepoint mutex — wraps the
	//      worker face-load chain (BSTaskThread_Runnable → sub_523220 →
	//      sub_9F88B0 → sub_5547F0) so it can't read FGP slots while the main
	//      thread is mid-mutation. CRITICAL_SECTION; uncontended cost ~10ns.
	//
	//   Layer 3. AgeMorphTable validation-failure redirect — replaces call to
	//      _invalid_parameter at 0x006EDDD4 with jmp to existing "return 0.0"
	//      early-exit path at 0x006EDD8F. Independent CRT-fail-fast bug class
	//      that can fire from any code path hitting AgeMorphTable::Lookup with
	//      an empty morph vector. 5-byte binary patch.
	//
	//   Layer 4. BSFaceGen_DoSomethingWithFaceGenNode FGP-validation hook —
	//      catches null FGP.models.data (ebp+0x78) and similar early null fields,
	//      bails out cleanly via skip path. Originally landed v512 as the
	//      headline fix; in v513-v515 testing it has not been observed firing,
	//      so it's now functioning as a safety net rather than primary defense.
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

	// Diagnostic: FGP-to-NPC mapping populated by HeadOverride hooks. Lets us
	// identify which NPC's face the engine is choking on when Layer 4 fires.
	static std::mutex                                 g_fgpMapLock;
	static std::unordered_map<void*, unsigned int>    g_fgpToNPC;

	// First-occurrence-per-FGP set. We dump full FGP state once per unique FGP
	// pointer; subsequent skips on the same FGP just bump the counter.
	static std::mutex                                 g_seenFGPLock;
	static std::unordered_set<void*>                  g_seenFGPs;

	void RegisterFGP_NPC(void* fgp, unsigned int npcRefID)
	{
		if (!fgp) return;
		std::lock_guard<std::mutex> g(g_fgpMapLock);
		g_fgpToNPC[fgp] = npcRefID;
	}

	void UnregisterFGP(void* fgp)
	{
		if (!fgp) return;
		std::lock_guard<std::mutex> g(g_fgpMapLock);
		g_fgpToNPC.erase(fgp);
		std::lock_guard<std::mutex> s(g_seenFGPLock);
		g_seenFGPs.erase(fgp);
	}

	static unsigned int LookupNPCForFGP(void* fgp)
	{
		std::lock_guard<std::mutex> g(g_fgpMapLock);
		auto it = g_fgpToNPC.find(fgp);
		return (it != g_fgpToNPC.end()) ? it->second : 0;
	}

	static bool IsFirstSkipForFGP(void* fgp)
	{
		std::lock_guard<std::mutex> g(g_seenFGPLock);
		return g_seenFGPs.insert(fgp).second;
	}

	// Dump every interesting FGP field. Layout per BlockheadInternals.h:103-152.
	static void DumpFGPState(void* fgp)
	{
		if (!fgp) return;
		char* p = (char*)fgp;
		unsigned int npcRefID = LookupNPCForFGP(fgp);

		// Header block
		void*        hair        = *(void**)(p + 0x60);
		unsigned int hairLen_raw = *(unsigned int*)(p + 0x68);  // float bits
		void*        eyes        = *(void**)(p + 0x6C);
		unsigned int female      = *(unsigned int*)(p + 0x70);

		// 4 NiTArrays (each 0x10 bytes): models, textures, nodeNames, sourceTextures
		// Layout per shadeMe FGP comment: data ptr is at NiTArray+0x04
		// (base is at +0x74, +0x84, +0x94, +0xA4 for the four arrays).
		auto dumpArray = [&](const char* name, unsigned int base) {
			unsigned int w0 = *(unsigned int*)(p + base + 0x00);
			void*        data = *(void**)(p + base + 0x04);
			unsigned int w2 = *(unsigned int*)(p + base + 0x08);
			unsigned int w3 = *(unsigned int*)(p + base + 0x0C);
			_MESSAGE("[RBRN] FGP-DUMP   %-12s vtbl=%08X data=%p w2=%08X w3=%08X",
			         name, w0, data, w2, w3);
		};

		// Tail block
		unsigned char useFGT     = *(unsigned char*)(p + 0xB4);
		void*         eyeLeft    = *(void**)(p + 0xB8);
		void*         eyeRight   = *(void**)(p + 0xBC);
		int           unkC0      = *(int*)(p + 0xC0);

		_MESSAGE("[RBRN] FGP-DUMP first-skip FGP=%p NPC=%08X female=%u hair=%p eyes=%p eyeL=%p eyeR=%p useFGT=%u unkC0=%d hairLenBits=%08X",
		         fgp, npcRefID, female, hair, eyes, eyeLeft, eyeRight,
		         (unsigned)useFGT, unkC0, hairLen_raw);
		dumpArray("models",      0x74);
		dumpArray("textures",    0x84);
		dumpArray("nodeNames",   0x94);
		dumpArray("srcTextures", 0xA4);
	}

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
	// Return-address bucket — log each unique caller RA only once.
	static std::mutex                                 g_seenRALock;
	static std::unordered_set<void*>                  g_seenRAs;

	static void __cdecl hook_DoSomething(void* faceGenNode, void* faceGenParams)
	{
		// Capture caller's return address BEFORE any other work. Goes onto our
		// stack frame at the point we enter, so it's stable for this invocation.
		void* retAddr = _ReturnAddress();

		if (!faceGenParams) {
			LONG n = InterlockedIncrement(&s_doSomethingSkipCount);
			if (n <= 5 || (n % 1000) == 0) {
				_MESSAGE("[RBRN] DoSomething skip #%ld: null FGP retAddr=%p", n, retAddr);
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
				_MESSAGE("[RBRN] DoSomething skip #%ld: null array (models=%p textures=%p third=%p) FGP=%p retAddr=%p",
				         n, models, textures, third, faceGenParams, retAddr);
			}
			// Diagnostic: dump full FGP state on first skip per unique FGP.
			if (IsFirstSkipForFGP(faceGenParams)) {
				DumpFGPState(faceGenParams);
			}
			// Diagnostic: log each unique caller return-address once.
			bool firstRA;
			{
				std::lock_guard<std::mutex> g(g_seenRALock);
				firstRA = g_seenRAs.insert(retAddr).second;
			}
			if (firstRA) {
				_MESSAGE("[RBRN] FGP-DUMP   NEW caller-RA=%p (called from this site for the first time)", retAddr);
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
