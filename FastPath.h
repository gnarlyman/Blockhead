#pragma once

#include <unordered_set>
#include <string>

// =====================================================================================
// [RBRN] Fix 11: hot-path early-return infrastructure.
//
// At plugin load, recursively scan Blockhead's override directories and build sets of:
//  - NPC formIDs (low 24 bits) that have any PerNPC override
//  - lowercased race names that have any PerRace override
//
// Hot hooks (SwapFaceGenHeadData, SwapRaceBodyModel, etc.) consult these sets at entry
// and fast-return for actors with no applicable override. This drops typical per-call
// latency from ~5µs (filesystem agent walks) to ~5ns (set lookup + branch), which is
// below the engine's hazard-pointer race threshold per the 2026-05-04 investigation.
// =====================================================================================

namespace FastPath
{
	// Run once at OBSEPlugin_Load before any patches are installed.
	void ScanAtStartup();

	// Returns true if any PerNPC head override exists for this NPC formID.
	// Match is on low 24 bits (local formID) — handles different load orders.
	bool HeadHasPerNPC(unsigned int npcRefID);

	// Returns true if any PerRace head override exists for this race name.
	// Race name match is case-insensitive.
	bool HeadHasPerRace(const char* raceName);

	// Returns true if the race's mesh dir (Meshes\Characters\<race>\) contains any file
	// matching `*_M.<ext>` or `*_F.<ext>` patterns — i.e. PerRace head gender-variant
	// overrides exist for this race. Combined with HeadHasPerNPC/HeadHasPerRace, lets
	// SwapFaceGenHeadData fast-path bail when no override of any kind is possible.
	bool HeadRaceHasGenderVariants(const char* raceName);

	// Same for body overrides.
	bool BodyHasPerNPC(unsigned int npcRefID);
	bool BodyHasPerRace(const char* raceName);
}
