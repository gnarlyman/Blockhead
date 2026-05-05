#pragma once

// =====================================================================================
// [RBRN] Engine race fix — surviving stack after 2026-05-04 strip + relayer.
//
// Three layers in this file (Fix 9 retry-loop guard lives in HeadOverride.cpp):
//
//   - sub_52DED0 worker face-load chokepoint mutex: serializes the BSFaceGen
//     worker chain (BSTaskThread_Runnable → sub_523220 → sub_9F88B0 →
//     sub_5547F0) against main-thread FGP mutation. Without this, mounted-NPC
//     patrol streams crash on sub_5547F0+0x2E9 when the worker reads FGP slots
//     mid-update.
//
//   - AgeMorphTable validation-failure redirect (0x006EDDD4 -> 0x006EDD8F):
//     5-byte binary patch that reroutes a CRT _invalid_parameter call (process
//     fail-fast) to the function's own existing return-0.0 early-exit.
//     Different bug class from the FGP corruption — fires from any code path
//     hitting AgeMorphTable::Lookup with an empty morph vector.
//
//   - BSFaceGen_DoSomething FGP validator hook: validates the FGP's
//     models.data / textures.data / third array pointers (offsets 0x78, 0x88,
//     0x98) at function entry; bails cleanly if any are null. Originally
//     thought to be the headline fix; in v513-v515 testing has not been
//     observed firing, so functioning as a safety net for the corrupt-FGP code
//     path that the chokepoint mutex doesn't cover.
//
// Removed in audit: LFM bucket-array NOPs, per-FGP lock map. See git history.
// =====================================================================================

namespace EngineRaceFix
{
	// Apply the surviving fixes. Returns true on success.
	bool Install();

	// Detach the Detours hook (binary patch is left in place; runtime never
	// unloads OBSE plugins anyway).
	void Uninstall();

	// Diagnostic: register FGP-to-NPC mapping so the Layer 4 skip hook can
	// identify which NPC is being processed. Called from
	// HeadOverride::DoTESRaceGetFaceGenHeadParametersHook after the engine's
	// GetFaceGenHeadParameters has populated the FGP. UnregisterFGP is called
	// from the FGP destructor hook.
	void RegisterFGP_NPC(void* fgp, unsigned int npcRefID);
	void UnregisterFGP(void* fgp);
}
