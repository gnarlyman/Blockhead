#include "FaceGenDataScanner.h"
#include "BlockheadInternals.h"

#include "obse/GameData.h"
#include "obse/GameForms.h"

#include <cstdio>

namespace FaceGenDataScanner
{
	static bool s_scanned = false;

	static const char* SafeEditorID(TESForm* form)
	{
		if (!form) return "<null>";
		const char* eid = form->GetEditorID();
		return (eid && eid[0]) ? eid : "<no-edid>";
	}

	static const char* PluginName(TESForm* form)
	{
		if (!form) return "<null>";
		UInt8 modIdx = (UInt8)((form->refID >> 24) & 0xFF);
		if (modIdx == 0xFF) return "<runtime>";
		const char* name = (*g_dataHandler)->GetNthModName(modIdx);
		return name ? name : "<unknown>";
	}

	// Engine layout (verified against sub_552990's 2x2 nested loop with stride 0x18):
	// 4 entries x 0x18 bytes each = 0x60 total. Each entry: 6 UInt32 fields.
	// The engine bail-skip checks fields[+0] and fields[+4] (likely image width/height).
	// If either is zero, the destination helper slot is zeroed -> downstream empty FGP.
	static bool HasZeroDimsAnywhere(const UInt8* base, UInt32 offset)
	{
		for (int i = 0; i < 4; i++)
		{
			const UInt32* slot = (const UInt32*)(base + offset + i * 0x18);
			if (slot[0] == 0 || slot[1] == 0) return true;
		}
		return false;
	}

	static void ScanRace(TESRace* race, FILE* log)
	{
		if (!race) return;
		const UInt8* base = (const UInt8*)race;
		for (int i = 0; i < 4; i++)
		{
			const UInt32* slot = (const UInt32*)(base + 0x29C + i * 0x18);
			if (slot[0] == 0 || slot[1] == 0)
			{
				fprintf(log, "RACE  %08X  unk12[%d]  v0=%08X  v1=%08X  v2=%08X  v3=%08X  v4=%08X  v5=%08X  EDID=%s  PLUGIN=%s\n",
					race->refID, i,
					slot[0], slot[1], slot[2], slot[3], slot[4], slot[5],
					SafeEditorID(race), PluginName(race));
			}
		}
	}

	static void ScanNPC(TESNPC* npc, FILE* log)
	{
		if (!npc) return;
		const UInt8* base = (const UInt8*)npc;
		const UInt32 offsets[2] = { 0x108, 0x168 };
		const char* names[2]    = { "unk1", "unk2" };
		for (int o = 0; o < 2; o++)
		{
			for (int i = 0; i < 4; i++)
			{
				const UInt32* slot = (const UInt32*)(base + offsets[o] + i * 0x18);
				if (slot[0] == 0 || slot[1] == 0)
				{
					TESRace* race = InstanceAbstraction::GetNPCRace(npc);
					fprintf(log, "NPC   %08X  %s[%d]  v0=%08X  v1=%08X  v2=%08X  v3=%08X  v4=%08X  v5=%08X  EDID=%s  PLUGIN=%s  RACE=%08X(%s)\n",
						npc->refID, names[o], i,
						slot[0], slot[1], slot[2], slot[3], slot[4], slot[5],
						SafeEditorID(npc), PluginName(npc),
						race ? race->refID : 0,
						race ? SafeEditorID(race) : "<null>");
				}
			}
		}
	}

	void Scan(void)
	{
		if (s_scanned) return;
		s_scanned = true;

		FILE* log = fopen("Blockhead-FaceGenScan.log", "w");
		if (!log)
		{
			_MESSAGE("[Scanner] failed to open Blockhead-FaceGenScan.log for write");
			return;
		}

		fprintf(log, "Blockhead FaceGen Data Scanner\n");
		fprintf(log, "==============================\n");
		fprintf(log, "Looks for ZERO at +0 or +4 of each 4x0x18 slot in:\n");
		fprintf(log, "  TESRace  +0x29C  unk12[0..3]\n");
		fprintf(log, "  TESNPC   +0x108  unk1[0..3]\n");
		fprintf(log, "  TESNPC   +0x168  unk2[0..3]\n");
		fprintf(log, "  (Engine layout: 4 entries x 0x18 bytes; sub_552990 reads each entry's +0 and +4 as numeric dims)\n");
		fprintf(log, "\n");

		if (!g_dataHandler || !*g_dataHandler)
		{
			fprintf(log, "ERROR: g_dataHandler not initialized; aborting.\n");
			fclose(log);
			_MESSAGE("[Scanner] g_dataHandler not initialized");
			return;
		}

		// ---- Races (linked-list traversal: races is DataHandler::Node<TESRace>) ----
		fprintf(log, "=== TESRace.unk12 (+0x29C) ===\n");
		UInt32 raceCount = 0, raceBad = 0;
		for (DataHandler::Node<TESRace>* node = &(*g_dataHandler)->races;
			node;
			node = node->next)
		{
			TESRace* race = node->data;
			if (!race) continue;
			raceCount++;
			if (HasZeroDimsAnywhere((const UInt8*)race, 0x29C))
			{
				raceBad++;
				ScanRace(race, log);
			}
		}
		fprintf(log, "  -> %u/%u races with NULL pair(s)\n\n", raceBad, raceCount);

		// ---- NPCs (via boundObjects list) ----
		fprintf(log, "=== TESNPC.unk1 (+0x108) and TESNPC.unk2 (+0x168) ===\n");
		UInt32 npcCount = 0, npcBad = 0;
		BoundObjectListHead* head = (*g_dataHandler)->boundObjects;
		if (!head)
		{
			fprintf(log, "  WARN: boundObjects head is NULL; skipping NPC scan.\n");
		}
		else
		{
			for (TESBoundObject* obj = head->first; obj; obj = obj->next)
			{
				if (obj->typeID != kFormType_NPC) continue;
				TESNPC* npc = (TESNPC*)obj;
				npcCount++;

				bool bad =
					HasZeroDimsAnywhere((const UInt8*)npc, 0x108) ||
					HasZeroDimsAnywhere((const UInt8*)npc, 0x168);
				if (bad)
				{
					npcBad++;
					ScanNPC(npc, log);
				}
			}
		}
		fprintf(log, "  -> %u/%u NPCs with NULL pair(s)\n", npcBad, npcCount);

		fclose(log);
		_MESSAGE("[Scanner] wrote Blockhead-FaceGenScan.log: races %u/%u, npcs %u/%u",
			raceBad, raceCount, npcBad, npcCount);
	}
}
