#include "HeadOverride.h"
#include "FastPath.h"

#include <atomic>
#include <mutex>
#include <unordered_set>

ScriptedActorAssetOverrider<ScriptedTextureOverrideData>		ScriptHeadOverrideAgent::TextureOverrides;
ScriptedActorAssetOverrider<ScriptedModelOverrideData>			ScriptHeadOverrideAgent::MeshOverrides;
FaceGenAgeTextureOverrider										FaceGenAgeTextureOverrider::Instance;

// =====================================================================================
// [RBRN] Fix 2: per-FGP allocator-ownership tracking.
//
// Blockhead writes its own FormHeap-allocated TESModel/TESTexture/TESHair pointers into
// FaceGenHeadParameters slots. The engine's dtor (DoFaceGenHeadParametersDtorHook) walks
// every slot and unconditionally FormHeap_Frees the pointer found there. Two failure modes:
//   (a) wrong-allocator free: the engine has re-populated a slot with an engine-owned
//       pointer (different allocator) since Blockhead's last swap.
//   (b) double-free: the engine queues the same FGP for two dtor calls (BSTask + main).
//
// Tracking only the pointers Blockhead installed (and consuming them on free) closes both
// failure modes simultaneously: untracked pointers are left alone (engine frees its own),
// and a pointer freed once is removed from the set so subsequent dtor calls treat it as
// engine-owned and skip it.
// =====================================================================================
static std::mutex                                                                g_OwnedPointersLock;
static std::unordered_map<FaceGenHeadParameters*, std::unordered_set<void*>>     g_OwnedPointers;

// [RBRN] Fix 11: atomic count of tracked pointers across all FGPs. Hot path can check
// this with a single relaxed atomic load (no lock acquisition) and skip the dtor's lock
// + iteration when nothing was ever allocated. With Fix 8's in-place mutation, this is
// the common case for nearly all actors.
static std::atomic<int>                                                          g_OwnedPointersCount{0};

static void TrackOwnedPointer(FaceGenHeadParameters* fgp, void* ptr)
{
	if (!fgp || !ptr) return;
	std::lock_guard<std::mutex> g(g_OwnedPointersLock);
	if (g_OwnedPointers[fgp].insert(ptr).second) {
		g_OwnedPointersCount.fetch_add(1, std::memory_order_relaxed);
	}
}

static bool ConsumeOwnedPointer(FaceGenHeadParameters* fgp, void* ptr)
{
	if (!fgp || !ptr) return false;
	std::lock_guard<std::mutex> g(g_OwnedPointersLock);
	auto fgpIt = g_OwnedPointers.find(fgp);
	if (fgpIt == g_OwnedPointers.end()) return false;
	bool erased = fgpIt->second.erase(ptr) > 0;
	if (erased) g_OwnedPointersCount.fetch_sub(1, std::memory_order_relaxed);
	if (fgpIt->second.empty())
		g_OwnedPointers.erase(fgpIt);
	return erased;
}

static void DropAllOwnedFor(FaceGenHeadParameters* fgp)
{
	if (!fgp) return;
	std::lock_guard<std::mutex> g(g_OwnedPointersLock);
	auto fgpIt = g_OwnedPointers.find(fgp);
	if (fgpIt != g_OwnedPointers.end()) {
		g_OwnedPointersCount.fetch_sub((int)fgpIt->second.size(), std::memory_order_relaxed);
		g_OwnedPointers.erase(fgpIt);
	}
}

// =====================================================================================
// [RBRN] Fix 3: per-FGP critical section.
//
// Even with allocator tracking, BSTaskManager thread can read FaceGenParams->models.data[i]
// at the exact instant the main thread is between "free old, write new" steps. For one
// machine instruction's worth, the pointer is stale/freed and a deref crashes. A mutex
// keyed by FGP* serializes swap and dtor against each other so neither can interleave.
//
// Scope is intentionally narrow: only the FGP-mutating critical region is locked, not the
// entire SwapFaceGenHeadData body (which also does file I/O for override probing — held
// over a mutex would serialize all FaceGen across threads needlessly). However, the plan
// says wrap the whole body — and uncontended std::mutex on Windows is a SRWLOCK (~10-30ns
// per acquire) and FaceGen frequencies are low, so the simpler "wrap whole body" wins on
// readability without measurable perf cost. Reverting to fine-grained locking is a simple
// follow-up if profiling ever shows contention.
//
// std::unordered_map element references are stable across insert/rehash (only iterators
// invalidate), so returning a reference to the mapped mutex is safe for callers to hold
// past subsequent map insertions.
// =====================================================================================
static std::mutex                                              g_FGPLocksMutex;
static std::unordered_map<FaceGenHeadParameters*, std::mutex>  g_FGPLocks;

static std::mutex& GetFGPLock(FaceGenHeadParameters* fgp)
{
	std::lock_guard<std::mutex> g(g_FGPLocksMutex);
	return g_FGPLocks[fgp];  // default-constructs on miss
}

const std::vector<const char*> ActorHeadAssetData::ValidComponentNames{
	"Head",
	"EarsMale",
	"EarsFemale",
	"Mouth",
	"TeethLower",
	"TeethUpper",
	"Tongue",
	"EyesLeft",
	"EyesRight",
};
const char* ActorHeadAssetData::OverrideFolderName = "HeadAssetOverrides";


ActorHeadAssetData::ActorHeadAssetData( UInt32 Type, AssetComponentT Component, TESNPC* Actor, const char* Path ) :
	IActorAssetData(Type, Component, Actor, Path)
{
	;//
}

bool ActorHeadAssetData::IsValid( void ) const
{
	switch (AssetComponent)
	{
	case FaceGenHeadParameters::kFaceGenData_Head:
	case FaceGenHeadParameters::kFaceGenData_EarsMale:
	case FaceGenHeadParameters::kFaceGenData_EarsFemale:
	case FaceGenHeadParameters::kFaceGenData_Mouth:
	case FaceGenHeadParameters::kFaceGenData_TeethLower:
	case FaceGenHeadParameters::kFaceGenData_TeethUpper:
	case FaceGenHeadParameters::kFaceGenData_Tongue:
		return true;
	case FaceGenHeadParameters::kFaceGenData_EyesLeft:
	case FaceGenHeadParameters::kFaceGenData_EyesRight:
		if (AssetType == kAssetType_Texture)				// eyes don't have corresponding textures, the TESEyes class takes care of that
			return false;
		else
			return true;
	default:
		return false;
	}
}

const char* ActorHeadAssetData::GetComponentName( void ) const
{
	switch (AssetComponent)
	{
	case FaceGenHeadParameters::kFaceGenData_Head:
		return "Head";
	case FaceGenHeadParameters::kFaceGenData_EarsMale:
		return "EarsMale";
	case FaceGenHeadParameters::kFaceGenData_EarsFemale:
		return "EarsFemale";
	case FaceGenHeadParameters::kFaceGenData_Mouth:
		return "Mouth";
	case FaceGenHeadParameters::kFaceGenData_TeethLower:
		return "TeethLower";
	case FaceGenHeadParameters::kFaceGenData_TeethUpper:
		return "TeethUpper";
	case FaceGenHeadParameters::kFaceGenData_Tongue:
		return "Tongue";
	case FaceGenHeadParameters::kFaceGenData_EyesLeft:
		return "EyesLeft";
	case FaceGenHeadParameters::kFaceGenData_EyesRight:
		return "EyesRight";
	default:
		return "Unknown";
	}
}

void ActorHeadAssetData::GetOverrideAgents( OverrideAgentListT& List )
{
	OverrideAgentHandleT Script(new ScriptHeadOverrideAgent(this));
	OverrideAgentHandleT NPC(new PerNPCHeadOverrideAgent(this));
	OverrideAgentHandleT Race(new PerRaceHeadOverrideAgent(this));
	OverrideAgentHandleT Default(new DefaultAssetOverrideAgent(this));

	List.push_back(NPC);
	List.push_back(Script);
	List.push_back(Race);
	List.push_back(Default);
}


ScriptHeadOverrideAgent::ScriptHeadOverrideAgent( IActorAssetData* Data ) :
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
	}

	SME_ASSERT(OverrideManager);
}

bool PerNPCHeadOverrideAgent::GetEnabled( void ) const
{
	switch (Data->AssetType)
	{
	case IActorAssetData::kAssetType_Texture:
		return Settings::kHeadOverrideTexturePerNPC.GetData().i != 0;
	case IActorAssetData::kAssetType_Model:
		return Settings::kHeadOverrideModelPerNPC.GetData().i != 0;
	default:
		return false;
	}
}

const char* PerNPCHeadOverrideAgent::GetOverrideSourceDirectory( void ) const
{
	return "Characters\\HeadAssetOverrides\\PerNPC";
}

PerNPCHeadOverrideAgent::PerNPCHeadOverrideAgent( IActorAssetData* Data ) :
	IPerNPCAssetOverrideAgent(Data)
{
	;//
}

bool PerRaceHeadOverrideAgent::GetEnabled( void ) const
{
	switch (Data->AssetType)
	{
	case IActorAssetData::kAssetType_Texture:
		return Settings::kHeadOverrideTexturePerRace.GetData().i != 0;
	case IActorAssetData::kAssetType_Model:
		return Settings::kHeadOverrideModelPerRace.GetData().i != 0;
	default:
		return false;
	}
}

const char* PerRaceHeadOverrideAgent::GetOverrideSourceDirectory( void ) const
{
	// only used when overriding body parts without default assets
	return "Characters\\HeadAssetOverrides\\PerRace";
}

PerRaceHeadOverrideAgent::PerRaceHeadOverrideAgent( IActorAssetData* Data ) :
	IPerRaceAssetOverrideAgent(Data)
{
	;//
}

bool PerRaceHeadOverrideAgent::GetComponentGenderVariant( void ) const
{
	switch (Data->AssetComponent)
	{
	case FaceGenHeadParameters::kFaceGenData_EarsMale:
	case FaceGenHeadParameters::kFaceGenData_EarsFemale:
		return true;
	default:
		return false;
	}
}

bool PerRaceHeadOverrideAgent::Query( std::string& OutOverridePath )
{
	bool Result = false;

	if (GetEnabled())
	{
		char Buffer[MAX_PATH] = {0};
		const char* PathSuffix = Data->GetComponentName();
		const char* BaseDir = Data->GetRootDirectory();
		const char* GenderPath = NULL;
		if (InstanceAbstraction::GetNPCFemale(Data->Actor))
			GenderPath = "F";
		else
			GenderPath = "M";

		if (Data->AssetPath && strlen(Data->AssetPath))
		{
			// per-race handling is mostly unnecessary for head parts
			// we only handle those that are in need of gender variance
			if (GetComponentGenderVariant() == false)
			{
				std::string OriginalPath(Data->AssetPath);

				// remove extension
				OriginalPath.erase(OriginalPath.length() - 4, 4);
				FORMAT_STR(Buffer, "%s\\%s_%s.%s", BaseDir, OriginalPath.c_str(), GenderPath, Data->GetFileExtension());
				DEBUG_MESSAGE("Checking override path %s for NPC '%s' (%08X)", Buffer, InstanceAbstraction::GetFormName(Data->Actor), Data->Actor->refID);

				if (InstanceAbstraction::FileFinder::GetFileExists(Buffer))
				{
					Result = true;
					FORMAT_STR(Buffer, "%s_%s.%s", OriginalPath.c_str(), GenderPath, Data->GetFileExtension());
					OutOverridePath = Buffer;
				}
			}
		}
		else
		{
			// fallback to the override directory when there's no default path
			const char* RaceName = InstanceAbstraction::GetFormName(Data->Race);
			if (RaceName)
			{
				// in the case ears, the gender component in the path must match that of the body part
				// for instance, EarsFemale overrides must be placed in the 'F' directory
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
	}

	return Result;
}
const char* FaceGenAgeTextureOverrider::kOverrideSourceDirectory = "Characters\\AgeTextureOverrides";

FaceGenAgeTextureOverrider::FaceGenAgeTextureOverrider() :
	OverriddenHeadTextures(),
	ScriptOverrides(),
	Lock()
{
	;//
}

FaceGenAgeTextureOverrider::~FaceGenAgeTextureOverrider()
{
	OverriddenHeadTextures.clear();
	ResetAgeTextureScriptOverrides();
}

void FaceGenAgeTextureOverrider::TrackHeadOverride( Texture MutatedTexture, const char* OriginalPath )
{
	ScopedLock Guard(Lock);

	SME_ASSERT(MutatedTexture && OriginalPath);
	// [RBRN] Fix 8: with in-place mutation, the same TESTexture pointer may be re-mutated on
	// a later swap. Allow re-tracking — overwrite any existing entry.
	OverriddenHeadTextures[MutatedTexture] = OriginalPath;
}

void FaceGenAgeTextureOverrider::UntrackHeadOverride( Texture MutatedTexture )
{
	ScopedLock Guard(Lock);

	if (OverriddenHeadTextures.count(MutatedTexture))
		OverriddenHeadTextures.erase(MutatedTexture);
}

void FaceGenAgeTextureOverrider::RegisterAgeTextureScriptOverride( TESNPC* NPC, const char* BasePath )
{
	ScopedLock Guard(Lock);

	SME_ASSERT(NPC && BasePath);

	ScriptOverrides[NPC->refID] = BasePath;
}

void FaceGenAgeTextureOverrider::UnregisterAgeTextureScriptOverride( TESNPC* NPC )
{
	ScopedLock Guard(Lock);

	SME_ASSERT(NPC);

	ScriptOverrides.erase(NPC->refID);
}

void FaceGenAgeTextureOverrider::ResetAgeTextureScriptOverrides( void )
{
	ScopedLock Guard(Lock);

	ScriptOverrides.clear();
}

bool FaceGenAgeTextureOverrider::TryGetClosestAgeTexture( std::string& OutPath, const char* BasePath, SInt32 Age, bool Female, bool UseGender ) const
{
	bool Result = false;

	DEBUG_MESSAGE("Fetching closest texture for age %d @ %s...", Age, BasePath);
#ifndef NDEBUG
	gLog.Indent();
#endif

	if (Age <= 100)
	{
		const char* GenderPath = (Female == false ? "M" : "F");
		char Buffer[MAX_PATH] = {0};
		FORMAT_STR(Buffer, "Textures\\%s%s%d.dds", BasePath, (UseGender ? GenderPath : ""), Age);
		if (InstanceAbstraction::FileFinder::GetFileExists(Buffer))
		{
			DEBUG_MESSAGE("Exact match for age - %s", Buffer);
			Result = true;
			OutPath = Buffer;
		}
		else
		{
			Age -= Age % 10;
			while (Age > 0)
			{
				FORMAT_STR(Buffer, "Textures\\%s%s%d.dds", BasePath, (UseGender ? GenderPath : ""), Age);
				if (InstanceAbstraction::FileFinder::GetFileExists(Buffer))
				{
					DEBUG_MESSAGE("Closest age = %d @ %s", Age, Buffer);
					Result = true;
					OutPath = Buffer;
					break;
				}

				Age -= 10;
			}
		}
	}

#ifndef NDEBUG
	gLog.Outdent();
#endif

	return Result;
}

std::string FaceGenAgeTextureOverrider::GetAgeTexturePath( TESNPC* NPC, SInt32 Age, const char* CurrentBasePath, Texture HeadTexture ) const
{
	ScopedLock Guard(Lock);

	SME_ASSERT(NPC);
	std::string Result;

	DEBUG_MESSAGE("Looking up age %d texture for NPC '%s' (%08X)...", Age, InstanceAbstraction::GetFormName(NPC), NPC->refID);
#ifndef NDEBUG
	gLog.Indent();
#endif

	while (true)
	{
		DEBUG_MESSAGE("Checking script overrides...");
#ifndef NDEBUG
		gLog.Indent();
#endif
		// scripted overrides have the highest priority
		if (ScriptOverrides.count(NPC->refID))
		{
			Result.clear();
			if (TryGetClosestAgeTexture(Result, ScriptOverrides.at(NPC->refID).c_str(), Age, false, false))
			{
#ifndef NDEBUG
				gLog.Outdent();
#endif
				break;
			}
		}
#ifndef NDEBUG
		gLog.Outdent();
		DEBUG_MESSAGE("Checking non-script overrides...");
		gLog.Indent();
#endif
		// check the override directory
		UInt32 FormID = NPC->refID & 0x00FFFFFF;
		TESFile* Plugin = InstanceAbstraction::GetOverrideFile(NPC, 0);
		if (Plugin)
		{
			char Buffer[MAX_PATH] = {0};
			FORMAT_STR(Buffer, "%s\\%s\\%08X_", kOverrideSourceDirectory, Plugin->name, FormID);
			Result.clear();
			if (TryGetClosestAgeTexture(Result, Buffer, Age, false, false))
			{
#ifndef NDEBUG
				gLog.Outdent();
#endif
				break;
			}
		}
#ifndef NDEBUG
		gLog.Outdent();
#endif

		std::string OrgBasePath(CurrentBasePath);
		if (OverriddenHeadTextures.count(HeadTexture))
		{
			// [RBRN] Fix 8: tracking map now stores the original path STRING directly
			// (since in-place mutation discards the original TESTexture pointer).
			OrgBasePath = OverriddenHeadTextures.at(HeadTexture);
			DEBUG_MESSAGE("Reset head asset path to %s", OrgBasePath.c_str());
		}
		else
			DEBUG_MESSAGE("No overrides, using current base path");

		// remove extension
		OrgBasePath.erase(OrgBasePath.length() - 4, 4);
		Result.clear();
		TryGetClosestAgeTexture(Result, OrgBasePath.c_str(), Age, InstanceAbstraction::GetNPCFemale(NPC), true);

		break;
	}

#ifndef NDEBUG
	gLog.Outdent();
#endif

	return Result;
}

void SwapFaceGenHeadData(TESRace* Race, FaceGenHeadParameters* FaceGenParams, TESNPC* NPC, bool FixingFaceNormals)
{
	if (!FaceGenParams) return;

	// [RBRN] Fix 11 REMOVED 2026-05-05: the FastPath early-return was incorrectly bailing
	// for NPCs whose overrides come through the script agent (ScriptHeadOverrideAgent,
	// runtime-registered via OBSE script commands like SetBodyAssetOverride / OCOv2's
	// face-load hooks). FastPath only scans HeadAssetOverrides\PerNPC and PerRace
	// directory trees — it doesn't see script-registered overrides. Bailing meant
	// vanilla NPCs lost their OCO faces and showed "Install Blockhead" placeholder.
	// Restoring shadeMe's original behavior: every actor's swap goes through all four
	// override agents (PerNPC, Script, PerRace, Default) as ApplyOverride iterates.

	// [RBRN] Fix 3: serialize swap against concurrent dtor + concurrent swap on the same FGP.
	std::lock_guard<std::mutex> fgpLock(GetFGPLock(FaceGenParams));

	// swap the head model/texture pointer with a newly allocated one
	// to allow for the changing of the asset paths
#ifndef NDEBUG
	if (NPC)
	{
		if (NPC->refID == 0x7)
			_MESSAGE("Generating FaceGen head for the player character...");
		else
			_MESSAGE("Generating FaceGen head for NPC %08X...", NPC->refID);
	}

	gLog.Indent();
	_MESSAGE("Name: %s\tRace: %s", InstanceAbstraction::GetFormName(NPC), InstanceAbstraction::GetFormName(Race));

	if (FixingFaceNormals)
		_MESSAGE("Fixing FaceGen normals...");

//	FaceGenParams->DebugDump();
#endif

	// [RBRN] Fix 1 reverted: restoring the sanity check.
	// Removing it caused SME_ASSERT(OrgModelPath) to fire on player FaceGen at game start
	// because some engine-owned slots are allocated-with-empty-path (e.g. EarsMale on female,
	// or eye slots handled via TESEyes/eyeLeft/eyeRight). The original code NULLed those so
	// they'd be skipped via `NonExtantModel = true` further down. SME_ASSERT calls _wassert
	// → abort() in release builds, killing the process before CrashLogger can flush.
	// sanity check, remove invalid model/texture pointers
	for (int i = FaceGenHeadParameters::kFaceGenData__BEGIN; i < FaceGenHeadParameters::kFaceGenData__END; i++)
	{
		InstanceAbstraction::TESModel::Instance ThisModel = (InstanceAbstraction::TESModel::Instance)
													FaceGenParams->models.data[i];
		InstanceAbstraction::TESTexture::Instance ThisTexture = (InstanceAbstraction::TESTexture::Instance)
													FaceGenParams->textures.data[i];

		if (ThisModel)
		{
			if (InstanceAbstraction::TESModel::GetPath(ThisModel)->m_data == NULL)
				FaceGenParams->models.data[i] = NULL;
		}

		if (ThisTexture)
		{
			if (InstanceAbstraction::TESTexture::GetPath(ThisTexture)->m_data == NULL)
				FaceGenParams->textures.data[i] = NULL;
		}
	}

	// [RBRN] Fix 8: in-place path mutation.
	//
	// The shipped Blockhead pattern was: alloc fresh TESModel/TESTexture, copy original path,
	// optionally apply override path, install in FGP slot. This changes the slot pointer on
	// every call, even when no override applies — and that pointer change is what triggers
	// the engine's retry-storm + downstream BSTaskManagerThread race that crashes mounted-NPC
	// scenes. (The pattern persisted from a much older Blockhead before the engine had the
	// retry behavior we now observe.)
	//
	// New pattern: mutate the engine's TESModel/TESTexture path in place via BSString::Set.
	// The slot pointer never changes — engine's mental model of FGP is preserved exactly as
	// vanilla. The only case still requiring slot mutation is non-extant slot + actual
	// override (rare; engine's slot was NULL but Blockhead wants to install an override).
	//
	// This subsumes Fix 7 (path-match short-circuit becomes implicit) and renders Fix 2's
	// allocator tracking dormant for the common path (no Blockhead allocation = nothing to
	// track). Fix 2 stays in place to handle the residual non-extant + override allocations.
	for (int i = FaceGenHeadParameters::kFaceGenData__BEGIN; i < FaceGenHeadParameters::kFaceGenData__END; i++)
	{
		InstanceAbstraction::TESModel::Instance OrgModel = (InstanceAbstraction::TESModel::Instance)
																FaceGenParams->models.data[i];
		InstanceAbstraction::TESTexture::Instance OrgTexture = (InstanceAbstraction::TESTexture::Instance)
																	FaceGenParams->textures.data[i];

		bool NonExtantModel = (OrgModel == NULL), NonExtantTexture = (OrgTexture == NULL);

		// ------------ MODEL ------------
		const char* OrgModelPath = NULL;
		if (NonExtantModel == false)
		{
			OrgModelPath = InstanceAbstraction::TESModel::GetPath(OrgModel)->m_data;
			SME_ASSERT(OrgModelPath);
		}

		if (NPC == NULL)
		{
			// Editor's Race edit dialog: leave the engine's TESModel alone. The original
			// "make a copy" dance was for a defunct CSE preview path; in-place leaves the
			// engine state unchanged, which is the conservative choice.
		}
		else
		{
			ActorHeadAssetData Data(ActorHeadAssetData::kAssetType_Model, i, NPC, OrgModelPath);
			std::string ResultPath;
			bool OverrideOp = ActorAssetOverriderKernel::Instance.ApplyOverride(&Data, ResultPath);

			if (NonExtantModel)
			{
				// Engine's slot is NULL. The only way to install an override is to allocate
				// a TESModel and put it in the slot — slot mutation unavoidable here.
				if (OverrideOp)
				{
					InstanceAbstraction::TESModel::Instance NewModel = InstanceAbstraction::TESModel::CreateInstance();
					InstanceAbstraction::TESModel::GetPath(NewModel)->Set(ResultPath.c_str());
					FaceGenParams->models.data[i] = (::TESModel*)NewModel;
					TrackOwnedPointer(FaceGenParams, NewModel);

					if (i == FaceGenHeadParameters::kFaceGenData_EyesLeft)
						FaceGenParams->eyeLeft = (::TESModel*)NewModel;
					else if (i == FaceGenHeadParameters::kFaceGenData_EyesRight)
						FaceGenParams->eyeRight = (::TESModel*)NewModel;
				}
				// no-op when no override on a non-extant slot
			}
			else if (OrgModelPath && _stricmp(ResultPath.c_str(), OrgModelPath) != 0)
			{
				// In-place path mutation: same TESModel pointer, new path. Engine sees no
				// pointer change — vanilla-equivalent slot stability.
				InstanceAbstraction::TESModel::GetPath(OrgModel)->Set(ResultPath.c_str());
			}
			// else: paths match (whether OverrideOp or not), nothing to do
		}

		// ------------ TEXTURE ------------
		const char* OrgTexturePath = NULL;
		if (NonExtantTexture == false)
		{
			OrgTexturePath = InstanceAbstraction::TESTexture::GetPath(OrgTexture)->m_data;
			SME_ASSERT(OrgTexturePath);
		}

		if (NPC == NULL)
		{
			// Editor mode: same as model — leave engine state alone.
		}
		else
		{
			ActorHeadAssetData Data(ActorHeadAssetData::kAssetType_Texture, i, NPC, OrgTexturePath);
			std::string ResultPath;
			bool OverrideOp = ActorAssetOverriderKernel::Instance.ApplyOverride(&Data, ResultPath);

			if (NonExtantTexture)
			{
				if (OverrideOp)
				{
					InstanceAbstraction::TESTexture::Instance NewTexture = InstanceAbstraction::TESTexture::CreateInstance();
					InstanceAbstraction::TESTexture::GetPath(NewTexture)->Set(ResultPath.c_str());
					FaceGenParams->textures.data[i] = (::TESTexture*)NewTexture;
					TrackOwnedPointer(FaceGenParams, NewTexture);
					// Non-extant + override: no original path to remember for age textures.
					// Age overlay defaults to the override path's base, which is reasonable.
				}
			}
			else if (OrgTexturePath && _stricmp(ResultPath.c_str(), OrgTexturePath) != 0)
			{
				// In-place mutation. For the head texture, remember the pre-override path
				// so the age-texture overlay system can fall back to it.
				if (i == FaceGenHeadParameters::kFaceGenData_Head && OverrideOp)
					FaceGenAgeTextureOverrider::Instance.TrackHeadOverride(OrgTexture, OrgTexturePath);

				InstanceAbstraction::TESTexture::GetPath(OrgTexture)->Set(ResultPath.c_str());
			}
		}
	}

	// [RBRN] Fix 8: hair gets the same in-place treatment. Original code unconditionally
	// allocated a new TESHair and replaced FaceGenParams->hair on every call, even when the
	// gender-variant override didn't apply. Now we mutate the engine's TESHair model/texture
	// paths in place only when the variant file actually exists.
	if (FaceGenParams->hair)
	{
		InstanceAbstraction::TESHair::Instance OldHair = (InstanceAbstraction::TESHair::Instance)FaceGenParams->hair;
		InstanceAbstraction::BSString* HairModelPath = InstanceAbstraction::TESModel::GetPath(InstanceAbstraction::TESHair::GetModel(OldHair));
		InstanceAbstraction::BSString* HairTexturePath = InstanceAbstraction::TESTexture::GetPath(InstanceAbstraction::TESHair::GetTexture(OldHair));

		if (Settings::kHeadOverrideHairGenderVariantModel().i)
		{
			if (HairModelPath->m_data)
			{
				std::string AssetPath(HairModelPath->m_data);
				AssetPath.erase(AssetPath.length() - 4, 4);		// remove extension
				if (FaceGenParams->female)
					AssetPath += "-F.nif";
				else
					AssetPath += "-M.nif";

				std::string OverridePath = "Meshes\\" + AssetPath;
#ifndef NDEBUG
				_MESSAGE("Checking hair model override at %s", OverridePath.c_str());
				gLog.Indent();
#endif
				if (InstanceAbstraction::FileFinder::GetFileExists(OverridePath.c_str()))
				{
					HairModelPath->Set(AssetPath.c_str());
					DEBUG_MESSAGE("Hair asset override applied");
				}
#ifndef NDEBUG
				gLog.Outdent();
#endif
			}
		}

		if (Settings::kHeadOverrideHairGenderVariantTexture().i)
		{
			if (HairTexturePath->m_data)
			{
				std::string AssetPath(HairTexturePath->m_data);
				AssetPath.erase(AssetPath.length() - 4, 4);		// remove extension
				if (FaceGenParams->female)
					AssetPath += "-F.dds";
				else
					AssetPath += "-M.dds";

				std::string OverridePath = "Textures\\" + AssetPath;
#ifndef NDEBUG
				_MESSAGE("Checking hair texture override at %s", OverridePath.c_str());
				gLog.Indent();
#endif
				if (InstanceAbstraction::FileFinder::GetFileExists(OverridePath.c_str()))
				{
					HairTexturePath->Set(AssetPath.c_str());
					DEBUG_MESSAGE("Hair asset override applied");
				}
#ifndef NDEBUG
				gLog.Outdent();
#endif
			}
		}

		// [RBRN] Fix 8: no slot replacement for hair — engine's TESHair pointer is preserved.
	}

#ifndef NDEBUG
	gLog.Outdent();
#endif
}

void __stdcall DoTESRaceGetFaceGenHeadParametersHook(TESRace* Race, FaceGenHeadParameters* FaceGenParams, TESNPC* NPC)
{
	// [RBRN] retry-loop guard: if the engine re-queues GetFaceGenHeadParameters with the
	// same (NPC, FaceGenParams) tuple consecutively, skip the swap. Some NPC base records
	// (e.g. OOO VirtueRider 000700CC) put the engine into a tight retry loop where each
	// SwapFaceGenHeadData causes the queued FaceGen task to be re-submitted, leading to a
	// BSTask-thread UAF crash on Set3D. Single-slot dedup is enough to break the cycle.
	static thread_local TESNPC* lastNPC = nullptr;
	static thread_local FaceGenHeadParameters* lastFGP = nullptr;
	static thread_local UInt32 dupeCount = 0;

	bool isDupe = (NPC == lastNPC && FaceGenParams == lastFGP);
	if (isDupe) {
		dupeCount++;
		if (dupeCount == 1) {
			_MESSAGE("[RBRN] RETRY-LOOP detected for NPC=%08X FGP=%p - hard-skipping (no engine call)",
				NPC ? NPC->refID : 0, FaceGenParams);
		}
		// [RBRN] v567 FIX: Fix 9 skipped sub_52CD50 (the original) entirely on dupes,
		// to avoid the engine queueing extra QueuedHead tasks. But the helper struct
		// the engine populates is on a worker-thread stack region that gets reused
		// across calls, AND its ctor (sub_527C90) does NOT initialize +0xB8/+0xBC
		// (eyeLeft/eyeRight). When we skip sub_52CD50, those fields retain stale
		// data — sometimes a Blockhead DLL address (0x73130C58 observed v566 #24).
		// Downstream sub_5547F0 then derefs the stale eyeLeft → AV.
		//
		// Fix: replicate sub_52CD50's CANONICAL eyeLeft/eyeRight write (race+0x188
		// and race+0x1A0, an interior pointer into TESRace.unk9[7] and unk9[8]).
		// Two pointer assignments. No QueuedHead enqueue (still skip thisCall).
		// FaceGenHeadParameters.eyeLeft is at +0xB8, eyeRight at +0xBC.
		if (Race && FaceGenParams) {
			*(UInt32*)((char*)FaceGenParams + 0xB8) = (UInt32)((char*)Race + 0x188);
			*(UInt32*)((char*)FaceGenParams + 0xBC) = (UInt32)((char*)Race + 0x1A0);
		}
		return;
	}

	if (dupeCount > 0) {
		_MESSAGE("[RBRN] RETRY-LOOP ended after %d skipped repeats for NPC=%08X",
			dupeCount, lastNPC ? lastNPC->refID : 0);
	}
	lastNPC = NPC;
	lastFGP = FaceGenParams;
	dupeCount = 0;

	// call original function to get the parameters
	thisCall<void>(InstanceAbstraction::kTESRace_GetFaceGenHeadParameters(), Race, NPC, FaceGenParams);

	SwapFaceGenHeadData(Race, FaceGenParams, NPC, false);
}

void __declspec(naked) TESRaceGetFaceGenHeadParametersHook(void)
{
	__asm
	{
		push	[esp + 0x4]
		push	[esp + 0xC]
		push	ecx
		call	DoTESRaceGetFaceGenHeadParametersHook
		retn	0x8
	}
}

void __stdcall DoFaceGenHeadParametersDtorHook(FaceGenHeadParameters* FaceGenParams)
{
	// [RBRN] Fix 11: ultra-fast path. With Fix 8's in-place mutation, we never allocate
	// for actors without override files (which is ~all actors). When that's true,
	// g_OwnedPointersCount stays at 0 and we have nothing to free; skip the lock + slot
	// iteration entirely and just chain to original. Single relaxed atomic load.
	if (g_OwnedPointersCount.load(std::memory_order_relaxed) == 0) {
		thisCall<void>(InstanceAbstraction::kFaceGenHeadParameters_Dtor(), FaceGenParams);
		return;
	}

	// [RBRN] Fix 3: serialize dtor against concurrent swap + concurrent dtor on the same FGP.
	// FaceGenParams is `this` from the engine's __thiscall — never NULL in practice.
	std::lock_guard<std::mutex> fgpLock(GetFGPLock(FaceGenParams));

	for (int i = FaceGenHeadParameters::kFaceGenData__BEGIN; i < FaceGenHeadParameters::kFaceGenData__END; i++)
	{
		if (i < FaceGenParams->models.numObjs)
		{
			InstanceAbstraction::TESModel::Instance SneakyBugger = (InstanceAbstraction::TESModel::Instance)
																FaceGenParams->models.data[i];

			// [RBRN] Fix 2: only free pointers Blockhead allocated. ConsumeOwnedPointer returns
			// false (and skips the FormHeap_Free) when the pointer is engine-owned (wrong-allocator
			// avoidance) or already freed (double-free avoidance).
			if (SneakyBugger && ConsumeOwnedPointer(FaceGenParams, SneakyBugger))
				InstanceAbstraction::TESModel::DeleteInstance(SneakyBugger);
		}

		if (i < FaceGenParams->textures.numObjs)
		{
			InstanceAbstraction::TESTexture::Instance SneakyBugger = (InstanceAbstraction::TESTexture::Instance)
																FaceGenParams->textures.data[i];

			if (SneakyBugger)
			{
				// remove the cached override data (no-op if engine-owned — only Blockhead-allocated
				// textures are ever entered into the FaceGenAgeTextureOverrider cache)
				FaceGenAgeTextureOverrider::Instance.UntrackHeadOverride(SneakyBugger);

				if (ConsumeOwnedPointer(FaceGenParams, SneakyBugger))
					InstanceAbstraction::TESTexture::DeleteInstance(SneakyBugger);
			}
		}
	}

	if (FaceGenParams->hair && ConsumeOwnedPointer(FaceGenParams, FaceGenParams->hair))
		InstanceAbstraction::TESHair::DeleteInstance(FaceGenParams->hair);

	// [RBRN] Fix 2: clear any residual ownership entries (defensive — shouldn't be any after
	// the loop above, but FGP buffers are recycled by the engine so we want a clean slate).
	DropAllOwnedFor(FaceGenParams);

	thisCall<void>(InstanceAbstraction::kFaceGenHeadParameters_Dtor(), FaceGenParams);
}

void __declspec(naked) FaceGenHeadParametersDtorHook(void)
{
	__asm
	{
		push	ecx
		call	DoFaceGenHeadParametersDtorHook
		retn
	}
}

void __cdecl DoBSFaceGenDoSomethingWithFaceGenNodeHook(TESNPC* NPC, NiNode* FaceGenNode, FaceGenHeadParameters* HeadParams)
{
	SwapFaceGenHeadData(InstanceAbstraction::GetNPCRace(NPC), HeadParams, NPC, true);

	cdeclCall<void>(InstanceAbstraction::kBSFaceGen_DoSomethingWithFaceGenNode(), FaceGenNode, HeadParams);
}

static UInt32		kBSFaceGenDoSomethingWithFaceGenNodeRetnAddr = 0;

void __declspec(naked) BSFaceGenDoSomethingWithFaceGenNodeHook(void)
{
	__asm
	{
		push	ebx
		call	DoBSFaceGenDoSomethingWithFaceGenNodeHook
		add		esp, 0x4				// account for the extra arg
		jmp		kBSFaceGenDoSomethingWithFaceGenNodeRetnAddr
	}
}

bool TryGetNPC(TESNPC* NPC)
{
	// ### HACKY HACK HACK HACKETT HACK
	// easier than mapping FaceGenHeadParam instances to their corresponding NPCs though
	bool Result = false;
	__try
	{
		switch (*((UInt32*)NPC))
		{
		case 0x0094561C:		// editor vtbl
		case 0x00A53DD4:		// runtime vtbl
			Result = true;
		}
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		Result = false;
	}

	return Result;
}

const char* __stdcall DoBSFaceGetAgeTexturePathHook(FaceGenHeadParameters* HeadParams,
													TESNPC* NPC,
													InstanceAbstraction::BSString* OutPath,
													UInt32 Gender,
													SInt32 Age,
													const char* BasePath)
{
	static const InstanceAbstraction::MemAddr kCallAddr = { 0x00551A00, 0x005845F0 };

	SME_ASSERT(HeadParams && HeadParams->textures.numObjs);

	InstanceAbstraction::TESTexture::Instance OverriddenTexture = (InstanceAbstraction::TESTexture::Instance)
																HeadParams->textures.data[FaceGenHeadParameters::kFaceGenData_Head];

	SME_ASSERT(OverriddenTexture);

	if (NPC)
	{
		if (TryGetNPC(NPC))
		{
			std::string AgeTexPath = FaceGenAgeTextureOverrider::Instance.GetAgeTexturePath(NPC, Age, BasePath, OverriddenTexture);
			if (AgeTexPath.length())
				OutPath->Set(AgeTexPath.c_str());
			else
				OutPath->Set("");

			return OutPath->m_data;
		}
		else
			DEBUG_MESSAGE("BSFaceGetAgeTexturePathHook - Bad NPC Pointer @ 0x%08X!", NPC);
	}

	return cdeclCall<const char*>(kCallAddr(), OutPath, Gender, Age, BasePath);
}

static UInt32		kBSFaceGetAgeTexturePathRetnAddr = 0;

#define _hhName		BSFaceGetAgeTexturePath
_hhBegin()
{
	__asm
	{
		mov		eax, [esp + 0x1C]						// ### HACK HACK - volatile stack space, can get overwritten
		push	eax
		test	InstanceAbstraction::EditorMode, 1
		jnz		EDITOR									// head param data is stored in a different register in the runtime

		push	ecx
		jmp		WEITER
	EDITOR:
		push	ebp
	WEITER:
		call	DoBSFaceGetAgeTexturePathHook
		jmp		kBSFaceGetAgeTexturePathRetnAddr		// our call will take care of the stack pointer
	}
}

void PatchHeadOverride( void )
{
	struct PatchSiteEins
	{
		InstanceAbstraction::MemAddr	TESRaceGetFaceGenHeadParameters;
		InstanceAbstraction::MemAddr	FaceGenHeadParametersDtor;

		PatchSiteEins(UInt32 GameA, UInt32 EditorA, UInt32 GameB, UInt32 EditorB)
		{
			TESRaceGetFaceGenHeadParameters.Game = GameA;
			TESRaceGetFaceGenHeadParameters.Editor = EditorA;

			FaceGenHeadParametersDtor.Game = GameB;
			FaceGenHeadParametersDtor.Editor = EditorB;
		}
	};

	std::vector<PatchSiteEins> HookLocations;		// 7 in-game and 3 in-editor

	HookLocations.push_back(PatchSiteEins(0x00528BF5, 0x004D9693, 0x00528C17, 0x004D9785));
	HookLocations.push_back(PatchSiteEins(0x00529301, 0x004DA46B, 0x00529356, 0x004DA48D));
	HookLocations.push_back(PatchSiteEins(0x0052966E, 0x004E739B, 0x00529702, 0x004E7405));
	HookLocations.push_back(PatchSiteEins(0x0052E03B, 0, 0x0052E0A5, 0));
	HookLocations.push_back(PatchSiteEins(0x005C7720, 0, 0x005C777A, 0));
	HookLocations.push_back(PatchSiteEins(0x005C7AC1, 0, 0x005C7B1B, 0));
	HookLocations.push_back(PatchSiteEins(0x005C936F, 0, 0x005C93C8, 0));

	for (int i = 0; i < HookLocations.size(); i++)
	{
		PatchSiteEins& Site = HookLocations[i];

		_DefineCallHdlr(PatchHookA, Site.TESRaceGetFaceGenHeadParameters(), TESRaceGetFaceGenHeadParametersHook);
		_DefineCallHdlr(PatchHookB, Site.FaceGenHeadParametersDtor(), FaceGenHeadParametersDtorHook);

		_MemHdlr(PatchHookA).WriteCall();
		_MemHdlr(PatchHookB).WriteCall();
	}

	struct PatchSiteZwei
	{
		InstanceAbstraction::MemAddr	BSFaceGenDoSomethingWithFaceGenNode;
		InstanceAbstraction::MemAddr	FaceGenHeadParametersDtor;

		PatchSiteZwei(UInt32 GameA, UInt32 EditorA, UInt32 GameB, UInt32 EditorB)
		{
			BSFaceGenDoSomethingWithFaceGenNode.Game = GameA;
			BSFaceGenDoSomethingWithFaceGenNode.Editor = EditorA;

			FaceGenHeadParametersDtor.Game = GameB;
			FaceGenHeadParametersDtor.Editor = EditorB;
		}
	};

	// special case for the facegen model normal fixing code
	// we only patch one of the two consecutive calls to the BSFaceGen function as the swapping is a one-time procedure
	const PatchSiteZwei kJustTheOne(0x005289BB, 0x004DA22F, 0x005289E3, 0x004DA257);
	_DefineJumpHdlr(PatchHookA, kJustTheOne.BSFaceGenDoSomethingWithFaceGenNode(), BSFaceGenDoSomethingWithFaceGenNodeHook);
	_DefineCallHdlr(PatchHookB, kJustTheOne.FaceGenHeadParametersDtor(), FaceGenHeadParametersDtorHook);

	_MemHdlr(PatchHookA).WriteJump();
	_MemHdlr(PatchHookB).WriteCall();

	kBSFaceGenDoSomethingWithFaceGenNodeRetnAddr = kJustTheOne.BSFaceGenDoSomethingWithFaceGenNode() + 0x5;

	const InstanceAbstraction::MemAddr	kBSFaceGetAgeTexturePath = { 0x00555457, 0x00587D4D };

	_DefineJumpHdlr(PatchHook, kBSFaceGetAgeTexturePath(), (UInt32)&BSFaceGetAgeTexturePathHook);
	_MemHdlr(PatchHook).WriteJump();

	kBSFaceGetAgeTexturePathRetnAddr = kBSFaceGetAgeTexturePath() + 0x8;
}

namespace HeadOverride
{
	void HandleLoadGame( void )
	{
		ScriptHeadOverrideAgent::TextureOverrides.Clear();
		ScriptHeadOverrideAgent::MeshOverrides.Clear();
		FaceGenAgeTextureOverrider::Instance.ResetAgeTextureScriptOverrides();
	}
}