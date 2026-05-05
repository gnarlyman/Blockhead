#pragma once

// =====================================================================================
// [RBRN] FaceGen data scanner — diagnostic for the BSFaceGen empty-NiTArray crash.
//
// Walks all loaded TESRaces and TESNPCs, reads the runtime fields whose NULL pointer
// pairs feed the crash chain (sub_552990 / sub_5528F0 silent-skip → empty out struct
// → sub_5551C0 AV at +0x179):
//
//   TESRace+0x29C: `unk12[]` — 2 entries × 0x30 bytes, ptrs at +0/+4 of each
//   TESNPC+0x108:  `unk1[]`  — 2 entries × 0x30 bytes, ptrs at +0/+4 of each
//   TESNPC+0x168:  `unk2[]`  — 2 entries × 0x30 bytes, ptrs at +0/+4 of each
//
// See `reference_facegen_call_chain.md` in user memory for the full RE.
//
// Output: `Data\OBSE\Plugins\Blockhead-FaceGenScan.log` listing every offending
// record with formID, EditorID, plugin name, slot index, and observed pointer values.
//
// Runs once per game session on the first save load OR new game (idempotent).
// =====================================================================================

namespace FaceGenDataScanner
{
	void Scan(void);
}
