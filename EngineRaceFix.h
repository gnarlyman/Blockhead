#pragma once

// =====================================================================================
// [RBRN] Engine race fix — bucket-array FormHeapFree neutralisation.
//
// Oblivion's IOManager / BSTaskManager / ModelLoader use a hazard-pointer + deferred-free
// lock-free protocol with intrinsic timing assumptions. Blockhead's mere PRESENCE adds
// enough latency to engine call paths that one thread can stash a bucket pointer just
// before another thread's rehash frees the old bucket array — UAF, crash signature
// `sub_432C30+0x44` (and `sub_432A60+0x29`).
//
// Fix: NOP the two bucket-array `call FormHeapFree` instructions inside the LFM resize
// routines (`sub_4328B0` and sister `sub_432740`). The arrays are leaked but the UAF
// window is closed. Per-node frees are left intact. Bounded leak (single-digit KB per
// session per chain).
//
// See EngineRaceFix.cpp for patch-site analysis and rationale.
// =====================================================================================

namespace EngineRaceFix
{
	// Apply the two 5-byte NOPs. Returns true on success.
	bool Install();

	// No-op stub (cannot un-NOP without restoring original bytes from disk; runtime
	// never unloads OBSE plugins).
	void Uninstall();
}
