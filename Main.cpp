#include "BlockheadInternals.h"
#include "Commands.h"
#include "HeadOverride.h"
#include "Sundries.h"
#include "BodyOverride.h"
#include "AnimationOverride.h"
#include "EquipmentOverride.h"
#include "EngineRaceFix.h"
#include "FastPath.h"
#include "VersionInfo.h"

#include <windows.h>
#include <string>

IDebugLog gLog("Blockhead.log");

// =====================================================================================
// [RBRN] Fix 10: conditional patching.
//
// Empirical finding (2026-05-04 session): Blockhead's mere PRESENCE via JMP-redirect
// hooks adds enough latency to engine call paths to violate the engine's hazard-pointer
// + deferred-free protocol around IOManager / BSTaskManager (sub_4328B0/sub_432C30/etc),
// crashing mounted-actor scenes. Disabling every Patch* removes the trigger entirely.
//
// Strategy: at plugin load, scan each subsystem's override directory tree. If no files
// exist for a subsystem, skip its Patch* call — Blockhead becomes invisible to the
// engine on that code path. For modlists with NO Blockhead overrides (vestigial Blockhead
// included only because OCO recommends it), this means full vanilla timing throughout.
// =====================================================================================
static bool DirHasAnyFile(const char* root, const char* extPattern)
{
	// Recursive scan. Returns true on first match. Pattern is e.g. "*.nif" or "*.kf".
	// MAX_PATH is fine — Oblivion doesn't support long paths anyway.
	char search[MAX_PATH];
	_snprintf_s(search, MAX_PATH, _TRUNCATE, "%s\\%s", root, extPattern);

	WIN32_FIND_DATAA fd;
	HANDLE hFind = FindFirstFileA(search, &fd);
	if (hFind != INVALID_HANDLE_VALUE) {
		// We only care if anything exists; a single hit is enough.
		FindClose(hFind);
		return true;
	}

	// Recurse into subdirectories.
	_snprintf_s(search, MAX_PATH, _TRUNCATE, "%s\\*", root);
	hFind = FindFirstFileA(search, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return false;

	bool found = false;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		if (fd.cFileName[0] == '.') continue;  // skip "." and ".."
		char child[MAX_PATH];
		_snprintf_s(child, MAX_PATH, _TRUNCATE, "%s\\%s", root, fd.cFileName);
		if (DirHasAnyFile(child, extPattern)) {
			found = true;
			break;
		}
	} while (FindNextFileA(hFind, &fd));
	FindClose(hFind);
	return found;
}

static bool ShouldPatchHeadOverride()
{
	// HeadAssetOverrides files: PerNPC/<plugin>/<formID>_<comp>.nif and PerRace/<gender>/<race>_<comp>.nif
	return DirHasAnyFile("Data\\Meshes\\Characters\\HeadAssetOverrides", "*.nif")
		|| DirHasAnyFile("Data\\Textures\\Characters\\HeadAssetOverrides", "*.dds");
}

static bool ShouldPatchBodyOverride()
{
	return DirHasAnyFile("Data\\Meshes\\Characters\\BodyAssetOverrides", "*.nif")
		|| DirHasAnyFile("Data\\Textures\\Characters\\BodyAssetOverrides", "*.dds");
}

static bool ShouldPatchAnimationOverride()
{
	// AnimationOverride only fires if files match Blockhead's "*_BLKD_<TAG>.kf" pattern,
	// otherwise the IDirectoryIterator returns 0 results per call. _BLKD_-tagged files
	// only — files in SpecialAnims/IdleAnims without the tag are loaded by the engine
	// natively, NOT by Blockhead.
	return DirHasAnyFile("Data\\Meshes\\Characters\\_male\\SpecialAnims", "*_BLKD_*.kf")
		|| DirHasAnyFile("Data\\Meshes\\Characters\\_male\\IdleAnims", "*_BLKD_*.kf");
}

static bool ShouldPatchEquipmentOverride()
{
	return DirHasAnyFile("Data\\Meshes\\Characters\\EquipmentAssetOverrides", "*.nif")
		|| DirHasAnyFile("Data\\Textures\\Characters\\EquipmentAssetOverrides", "*.dds");
}


static void LoadCallbackHandler(void * reserved)
{
	BodyOverride::HandleLoadGame(true);
	HeadOverride::HandleLoadGame();
	AnimOverride::HandleLoadGame();
	EquipmentOverride::HandleLoadGame();
}

static void SaveCallbackHandler(void * reserved)
{
	;//
}

static void NewGameCallbackHandler(void * reserved)
{
	BodyOverride::HandleLoadGame(false);
	HeadOverride::HandleLoadGame();
	AnimOverride::HandleLoadGame();
	EquipmentOverride::HandleLoadGame();
}

void BlockheadMessageHandler(OBSEMessagingInterface::Message* Msg)
{
	if (Msg->type == 'CSEI')
	{
		CSEInterface* Interface = (CSEInterface*)Msg->data;

		Interfaces::kCSEConsole = (CSEConsoleInterface*)Interface->InitializeInterface(CSEInterface::kCSEInterface_Console);
		Interfaces::kCSEIntelliSense = (CSEIntelliSenseInterface*)Interface->InitializeInterface(CSEInterface::kCSEInterface_IntelliSense);

		_MESSAGE("Received interface from CSE");

		RegisterCommandsWithCSE();
	}
}

void OBSEMessageHandler(OBSEMessagingInterface::Message* Msg)
{
	switch (Msg->type)
	{
	case OBSEMessagingInterface::kMessage_PostLoad:
		Interfaces::kOBSEMessaging->RegisterListener(Interfaces::kOBSEPluginHandle, "CSE", BlockheadMessageHandler);
		_MESSAGE("Registered to receive messages from CSE");

		break;
	case OBSEMessagingInterface::kMessage_PostPostLoad:
		_MESSAGE("Requesting an interface from CSE");
		Interfaces::kOBSEMessaging->Dispatch(Interfaces::kOBSEPluginHandle, 'CSEI', NULL, 0, "CSE");

		break;
	}
}

extern "C"
{
	bool OBSEPlugin_Query(const OBSEInterface * obse, PluginInfo * info)
	{
		_MESSAGE("Blockhead Initializing...");

		info->infoVersion =	PluginInfo::kInfoVersion;
		info->name =		"Blockhead";
		info->version =		PACKED_SME_VERSION;

		Interfaces::kOBSEPluginHandle = obse->GetPluginHandle();
		if(obse->obseVersion < 21)
		{
			_ERROR("OBSE version too old (got %08X expected at least %d)", obse->obseVersion, 21);
			return false;
		}

		InstanceAbstraction::EditorMode = false;

		if (obse->isEditor)
		{
			InstanceAbstraction::EditorMode = true;

			if (obse->editorVersion != CS_VERSION_1_2)
			{
				_MESSAGE("Unsupported editor version %08X", obse->oblivionVersion);
				return false;
			}
		}
		else
		{
			if (obse->oblivionVersion != OBLIVION_VERSION)
			{
				_MESSAGE("Unsupported runtime version %08X", obse->oblivionVersion);
				return false;
			}

			Interfaces::kOBSESerialization = (OBSESerializationInterface *)obse->QueryInterface(kInterface_Serialization);
			if (!Interfaces::kOBSESerialization)
			{
				_MESSAGE("Serialization interface not found");
				return false;
			}

			if (Interfaces::kOBSESerialization->version < OBSESerializationInterface::kVersion)
			{
				_MESSAGE("Incorrect serialization version found (got %08X need %08X)", Interfaces::kOBSESerialization->version, OBSESerializationInterface::kVersion);
				return false;
			}

			Interfaces::kOBSEArrayVar = (OBSEArrayVarInterface*)obse->QueryInterface(kInterface_ArrayVar);
			if (!Interfaces::kOBSEArrayVar)
			{
				_MESSAGE("Array interface not found");
				return false;
			}

			Interfaces::kOBSEScript = (OBSEScriptInterface*)obse->QueryInterface(kInterface_Script);
			if (!Interfaces::kOBSEScript)
			{
				_MESSAGE("Script interface not found");
				return false;
			}

			Interfaces::kOBSEIO = (OBSEIOInterface*)obse->QueryInterface(kInterface_IO);
			if (Interfaces::kOBSEIO == NULL)
			{
				_MESSAGE("IO interface not found");
				return false;
			}

			Interfaces::kOBSEStringVar = (OBSEStringVarInterface*)obse->QueryInterface(kInterface_StringVar);
			if (Interfaces::kOBSEStringVar == NULL)
			{
				_MESSAGE("String var interface not found");
				return false;
			}
		}

		Interfaces::kOBSEMessaging = (OBSEMessagingInterface*)obse->QueryInterface(kInterface_Messaging);
		if (Interfaces::kOBSEMessaging == NULL)
		{
			_MESSAGE("Messaging interface not found");
			return false;
		}

		return true;
	}

	bool OBSEPlugin_Load(const OBSEInterface * obse)
	{
		_MESSAGE("Initializing INI Manager");
		BlockheadINIManager::Instance.Initialize("Data\\OBSE\\Plugins\\Blockhead.ini", NULL);

		if (InstanceAbstraction::EditorMode == false)
		{
			Interfaces::kOBSESerialization->SetSaveCallback(Interfaces::kOBSEPluginHandle, SaveCallbackHandler);
			Interfaces::kOBSESerialization->SetLoadCallback(Interfaces::kOBSEPluginHandle, LoadCallbackHandler);
			Interfaces::kOBSESerialization->SetNewGameCallback(Interfaces::kOBSEPluginHandle, NewGameCallbackHandler);

			RegisterStringVarInterface(Interfaces::kOBSEStringVar);
		}
		else
		{
			Interfaces::kOBSEMessaging->RegisterListener(Interfaces::kOBSEPluginHandle, "OBSE", OBSEMessageHandler);
		}

		_MESSAGE("Pah! There's no pleasing some horses!\n\n");
		_MESSAGE("===== REBORN INSTRUMENTED BUILD %d.%d.%d.%d (Release) LOADED =====", VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION, VERSION_BUILD);
		_MESSAGE("[RBRN] Build identifier: gnarlyman/Blockhead investigation fork");
		gLog.Indent();


		RegisterCommands(obse);

		// [RBRN] Fix 11: pre-scan override directories to build fast-path sets BEFORE
		// installing patches. Hooks check the sets first; for actors with no override,
		// hooks fast-return in a few nanoseconds, well below the engine race threshold.
		FastPath::ScanAtStartup();

		// [RBRN] Refined Option A: NOP the engine's bucket-array FormHeapFree calls so
		// the LockFreeMap's deferred-free GC can never produce a UAF. See EngineRaceFix.cpp.
		EngineRaceFix::Install();

		// All patches installed unconditionally; Fix 11's fast-paths in the hook bodies
		// keep latency low enough that the engine's hazard-pointer protocol holds.
		PatchHeadOverride();
		PatchBodyOverride();
		PatchAnimationOverride();
		PatchEquipmentOverride();
		PatchSundries();

		return true;
	}

	BOOL WINAPI DllMain(HANDLE hDllHandle, DWORD dwReason, LPVOID lpreserved)
	{
		return TRUE;
	}
};

