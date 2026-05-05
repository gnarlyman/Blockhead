#pragma once

// =====================================================================================
// [RBRN] Engine race fix — four-layer stack (post-2026-05-05 re-validation).
//
// Four layers in this file (Fix 9 retry-loop guard lives in HeadOverride.cpp):
//
//   - LFM bucket-array FormHeapFree NOPs at 0x0043296E + 0x004327EC:
//     closes the use-after-free window in the LockFreeMap resize routines
//     (sub_4328B0 / sub_432740). Without this, extended play eventually
//     crashes at sub_4328B0+0x5A. The v518 strip-test that suggested this
//     was redundant turned out to be a too-short test; extended play in
//     v518 reproduced the original LFM crash. Bounded leak.
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
//     0x98) at function entry; bails cleanly if any are null. Primary defense
//     for the empty-NiTArray scenario from sub_52DED0 (see
//     feedback_facegen_storm_root_cause.md).
//
// Removed in audit: per-FGP g_FGPLocks lock-map (Fix 8's in-place mutation
// makes it unnecessary for the common case). See git history.
// =====================================================================================

namespace EngineRaceFix
{
	// Apply the surviving fixes. Returns true on success.
	bool Install();

	// Detach the Detours hook (binary patch is left in place; runtime never
	// unloads OBSE plugins anyway).
	void Uninstall();
}
