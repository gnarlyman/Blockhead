#include "BodyOverride.h"
#include "FastPath.h"

ScriptedActorAssetOverrider<ScriptedTextureOverrideData>		ScriptBodyOverrideAgent::TextureOverrides;
ScriptedActorAssetOverrider<ScriptedModelOverrideData>			ScriptBodyOverrideAgent::MeshOverrides;
ScriptedActorAssetOverrider<ScriptedModelOverrideData>			ScriptBodyOverrideAgent::FaceGenTextureOverrides;

const std::vector<const char*> ActorBodyAssetData::ValidComponentNames {
	"UpperBody",
	"LowerBody",
	"Hand",
	"Foot",
	"Tail",
};
const char* ActorBodyAssetData::OverrideFolderName = "BodyAssetOverrides";


ActorBodyAssetData::ActorBodyAssetData( UInt32 Type, AssetComponentT Component, TESNPC* Actor, const char* Path ) :
	IActorAssetData(Type, Component, Actor, Path)
{
	;//
}

bool ActorBodyAssetData::IsValid( void ) const
{
	switch (AssetComponent)
	{
	case kBodyPart_UpperBody:
	case kBodyPart_LowerBody:
	case kBodyPart_Hand:
	case kBodyPart_Foot:
	case kBodyPart_Tail:
		return true;
	default:
		return false;
	}
}

const char* ActorBodyAssetData::GetComponentName( void ) const
{
	switch (AssetComponent)
	{
	case kBodyPart_UpperBody:
		return "UpperBody";
	case kBodyPart_LowerBody:
		return "LowerBody";
	case kBodyPart_Hand:
		return "Hand";
	case kBodyPart_Foot:
		return "Foot";
	case kBodyPart_Tail:
		return "Tail";
	default:
		return "Unknown";
	}
}

void ActorBodyAssetData::GetOverrideAgents( OverrideAgentListT& List )
{
	OverrideAgentHandleT Script(new ScriptBodyOverrideAgent(this));
	OverrideAgentHandleT NPC(new PerNPCBodyOverrideAgent(this));
	OverrideAgentHandleT Race(new PerRaceBodyOverrideAgent(this));
	OverrideAgentHandleT Default(new DefaultAssetOverrideAgent(this));

	List.push_back(NPC);
	List.push_back(Script);
	List.push_back(Race);
	List.push_back(Default);
}

ScriptBodyOverrideAgent::ScriptBodyOverrideAgent( IActorAssetData* Data ) :
	IScriptAssetOverrideAgent(Data, NULL)
{
	switch (Data->AssetType)
	{
	case IActorAssetData::kAssetType_Texture:
		OverrideManager = &TextureOverrides;
		break;
	case IActorAssetData::kAssetType_Model:
		OverrideManager = &MeshOverrides;
		break;
	case IActorAssetData::kAssetType_BodyEGT:
		OverrideManager = &FaceGenTextureOverrides;
		break;
	}

	SME_ASSERT(OverrideManager);
}

bool PerNPCBodyOverrideAgent::GetEnabled( void ) const
{
	switch (Data->AssetType)
	{
	case IActorAssetData::kAssetType_Texture:
	case IActorAssetData::kAssetType_BodyEGT:
		return Settings::kBodyOverrideTexturePerNPC.GetData().i != 0;
	case IActorAssetData::kAssetType_Model:
		return Settings::kBodyOverrideModelPerNPC.GetData().i != 0;
	default:
		return false;
	}
}

const char* PerNPCBodyOverrideAgent::GetOverrideSourceDirectory( void ) const
{
	return "Characters\\BodyAssetOverrides\\PerNPC";
}

PerNPCBodyOverrideAgent::PerNPCBodyOverrideAgent( IActorAssetData* Data ) :
	IPerNPCAssetOverrideAgent(Data)
{
	;//
}

bool PerRaceBodyOverrideAgent::GetEnabled( void ) const
{
	switch (Data->AssetType)
	{
	case IActorAssetData::kAssetType_Texture:
		return Settings::kBodyOverrideTexturePerRace.GetData().i != 0;
	case IActorAssetData::kAssetType_Model:
	case IActorAssetData::kAssetType_BodyEGT:
		return Settings::kBodyOverrideModelPerRace.GetData().i != 0;
	default:
		return false;
	}
}

const char* PerRaceBodyOverrideAgent::GetOverrideSourceDirectory( void ) const
{
	return "Characters\\BodyAssetOverrides\\PerRace";
}

PerRaceBodyOverrideAgent::PerRaceBodyOverrideAgent( IActorAssetData* Data ) :
	IPerRaceAssetOverrideAgent(Data)
{
	;//
}

bool PerRaceBodyOverrideAgent::Query( std::string& OutOverridePath )
{
	bool Result = false;

	if (GetEnabled())
	{
		const char* RaceName = InstanceAbstraction::GetFormName(Data->Race);
		const char* PathSuffix = Data->GetComponentName();
		const char* BaseDir = Data->GetRootDirectory();
		const char* GenderPath = NULL;
		if (InstanceAbstraction::GetNPCFemale(Data->Actor))
			GenderPath = "F";
		else
			GenderPath = "M";

		if (RaceName)
		{
			char Buffer[MAX_PATH] = {0};
			FORMAT_STR(Buffer, "%s\\%s\\%s_%s.%s", GetOverrideSourceDirectory(), GenderPath, RaceName, PathSuffix, Data->GetFileExtension());

			DEBUG_MESSAGE("Checking override path %s for NPC '%s' (%08X)", Buffer, InstanceAbstraction::GetFormName(Data->Actor), Data->Actor->refID);

			std::string FullPath(BaseDir); FullPath += "\\" + std::string(Buffer);
			if (InstanceAbstraction::FileFinder::GetFileExists(FullPath.c_str()))
			{
				Result = true;
				OutOverridePath = Buffer;
			}
		}
	}

	return Result;
}

_DefineHookHdlr(TESRaceGetBodyTexture, 0x0052D4C9);
_DefineHookHdlr(TESRaceGetBodyModelA, 0x0047ABFA);
_DefineHookHdlr(TESRaceGetBodyModelB, 0x00523934);
_DefinePatchHdlr(TESRaceGetTailTexture, 0x0052D472);		// prevents tail texture lookup from failing prematurely if there wasn't a valid asset path
_DefineHookHdlr(TESRaceGetBodyEGT, 0x0052D612);

void __cdecl SwapRaceBodyTexture(TESRace* Race, UInt8 BodyPart, TESNPC* NPC, InstanceAbstraction::BSString* OutTexPath, const char* Format, const char* OrgTexPath)
{
	// [RBRN] Fix 11: hot-path early return. If neither this NPC nor its race has body
	// overrides, replicate the engine's vanilla behavior (write the original path) and
	// skip the agent walk entirely.
	if (NPC && Race &&
		!FastPath::BodyHasPerNPC(NPC->refID) &&
		!FastPath::BodyHasPerRace(InstanceAbstraction::GetFormName(Race)))
	{
		char OverrideTexPath[MAX_PATH] = {0};
		FORMAT_STR(OverrideTexPath, "Textures\\%s", OrgTexPath ? OrgTexPath : "");
		OutTexPath->Set(OverrideTexPath);
		return;
	}

	ActorBodyAssetData Data(ActorBodyAssetData::kAssetType_Texture, BodyPart, NPC, OrgTexPath);
	char OverrideTexPath[MAX_PATH] = {0};
	std::string ResultPath;

	ActorAssetOverriderKernel::Instance.ApplyOverride(&Data, ResultPath);
	FORMAT_STR(OverrideTexPath, "Textures\\%s", ResultPath.c_str());

	OutTexPath->Set(OverrideTexPath);
}

#define _hhName		TESRaceGetBodyTexture
_hhBegin()
{
	_hhSetVar(Retn, 0x0052D4CE);
	__asm
	{
		push	[ebp - 0x20]
		push	[ebp - 0x1C]
		push	edi
		call	SwapRaceBodyTexture
		add		esp, 0xC

		jmp		_hhGetVar(Retn)
	}
}

TESModel* __stdcall SwapRaceBodyModel(TESNPC* NPC, TESRace* Race, UInt32 Gender, UInt32 BodyPart)
{
	static std::unordered_map<std::string, TESModel*> ModelPathToModelBuffer;

	// get the model thingy
	TESModel* Original = thisCall<TESModel*>(0x0052BE80, Race, Gender, BodyPart);

	// [RBRN] Fix 11: hot-path early return. Vast majority of actors have no body overrides;
	// for them, return the engine's original pointer unchanged — same as shadeMe's existing
	// "no override and path matches" guard further down, but reached without ApplyOverride's
	// agent-walk + file-existence checks.
	if (NPC && Race &&
		!FastPath::BodyHasPerNPC(NPC->refID) &&
		!FastPath::BodyHasPerRace(InstanceAbstraction::GetFormName(Race)))
	{
		return Original;  // NULL is a valid return per the original semantics
	}

	bool NonExtantModel = (Original == NULL || Original->nifPath.m_data == NULL);
	ActorBodyAssetData Data(ActorBodyAssetData::kAssetType_Model, BodyPart, NPC, (NonExtantModel == false ? Original->nifPath.m_data : NULL));
	std::string ResultPath;
	bool OverrideOp = ActorAssetOverriderKernel::Instance.ApplyOverride(&Data, ResultPath);

	if (!OverrideOp && NonExtantModel)
		return NULL;
	else if (!NonExtantModel && !_stricmp(ResultPath.c_str(), Original->nifPath.m_data))
		return Original;

	// lookup the buffer for the override in the cache, allocate a new one if necessary
	std::string ResultPathLower(ResultPath);
	SME::StringHelpers::MakeLower(ResultPathLower);

	auto Match = ModelPathToModelBuffer.find(ResultPathLower);
	if (Match != ModelPathToModelBuffer.cend())
		return Match->second;

	auto NewBuffer = reinterpret_cast<TESModel*>(InstanceAbstraction::TESModel::CreateInstance());
	NewBuffer->nifPath.Set(ResultPath.c_str());
	ModelPathToModelBuffer.emplace(ResultPathLower, NewBuffer);

	return NewBuffer;
}

#define _hhName		TESRaceGetBodyModelA
_hhBegin()
{
	_hhSetVar(Retn, 0x0047ABFF);
	__asm
	{
		mov		eax, [esp + 0x14]			// muck around with the stack frame, then you're gonna have a BadTime�
		push	ecx
		push	eax
		call	SwapRaceBodyModel
		jmp		_hhGetVar(Retn)
	}
}

#define _hhName		TESRaceGetBodyModelB
_hhBegin()
{
	_hhSetVar(Retn, 0x00523939);
	__asm
	{
		push	ecx
		push	edi
		call	SwapRaceBodyModel
		jmp		_hhGetVar(Retn)
	}
}

TESModel* __stdcall SwapRaceBodyFaceGenModel(TESNPC* NPC, UInt32 BodyPart, TESModel* Original)
{
	static TESModel* kEGTModel = NULL;
	if (kEGTModel == NULL)
		kEGTModel = (TESModel*)InstanceAbstraction::TESModel::CreateInstance();

	if (Original == NULL)
		return Original;

	// [RBRN] Fix 11: hot-path early return. EGT path is body-asset-class; if no body
	// overrides apply for this NPC or its race, return engine's original unchanged.
	if (NPC) {
		TESRace* Race = InstanceAbstraction::GetNPCRace(NPC);
		if (!FastPath::BodyHasPerNPC(NPC->refID) &&
			!FastPath::BodyHasPerRace(Race ? InstanceAbstraction::GetFormName(Race) : NULL))
		{
			return Original;
		}
	}

	ActorBodyAssetData Data(ActorBodyAssetData::kAssetType_BodyEGT, BodyPart, NPC, Original->nifPath.m_data);
	std::string ResultPath;

	bool OverrideOp = ActorAssetOverriderKernel::Instance.ApplyOverride(&Data, ResultPath);
	kEGTModel->nifPath.Set(ResultPath.c_str());

	return kEGTModel;
}

#define _hhName		TESRaceGetBodyEGT
_hhBegin()
{
	_hhSetVar(Retn, 0x0052D617);
	__asm
	{
		add		esp, 0xC
		mov		eax, [esp + 0x8]
		push	esi
		push	ebp
		push	eax
		call	SwapRaceBodyFaceGenModel
		mov		esi, eax
		cmp		esi, 0
		jmp		_hhGetVar(Retn)
	}
}

void PatchBodyOverride( void )
{
	if (InstanceAbstraction::EditorMode == false)
	{
		_MemHdlr(TESRaceGetBodyTexture).WriteJump();
		_MemHdlr(TESRaceGetBodyModelA).WriteJump();
		_MemHdlr(TESRaceGetBodyModelB).WriteJump();
		_MemHdlr(TESRaceGetTailTexture).WriteUInt8(0xEB);
		_MemHdlr(TESRaceGetBodyEGT).WriteJump();
	}
}

namespace BodyOverride
{
	void FixPlayerBodyModel( void )
	{
		// the engine caches 3D body model data (as it doesn't expect the mesh to change after game init), causing mismatching models when loading a save game where the gender of the NPC/player has changed
		// easiest thing to do would be to quit to the main menu before loading the save, which flushes the cache
		// we'll just update the PC's model as it's the most ostentatious
		if (*g_thePlayer)
		{
			UInt8 State = (*g_thePlayer)->isThirdPerson;
			if (State == 0)
				(*g_thePlayer)->TogglePOV(false);

			(*g_thePlayer)->Update3D();

			if (State == 0)
				(*g_thePlayer)->TogglePOV(true);
		}
	}

	void HandleLoadGame( bool FixPlayer3D )
	{
		ScriptBodyOverrideAgent::TextureOverrides.Clear();
		ScriptBodyOverrideAgent::MeshOverrides.Clear();
		ScriptBodyOverrideAgent::FaceGenTextureOverrides.Clear();

		// ### calling FixPlayerBodyModel() here causes CTDs under certain conditions (when the PC's sitting, on a horse, etc)
		// let the user re-equip the player's equipment instead
	}
}