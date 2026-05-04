#include "FastPath.h"
#include "BlockheadInternals.h"

#include <windows.h>
#include <string>
#include <cctype>

namespace FastPath
{
	// Built once at startup, read-only thereafter. No locking needed in hot path.
	static std::unordered_set<unsigned int>  s_HeadPerNPC;
	static std::unordered_set<std::string>   s_HeadPerRace;
	static std::unordered_set<std::string>   s_HeadGenderVariantRaces;
	static std::unordered_set<unsigned int>  s_BodyPerNPC;
	static std::unordered_set<std::string>   s_BodyPerRace;

	static std::string ToLower(const char* s)
	{
		std::string r;
		if (s) for (; *s; ++s) r += (char)std::tolower((unsigned char)*s);
		return r;
	}

	// Parse formID from filename of form "<8-hex>_<component>.<ext>".
	// Returns the formID (low 24 bits) or 0 if filename doesn't match.
	static unsigned int ParseFormIDFromFilename(const char* filename)
	{
		if (!filename) return 0;
		// Filename must start with 8 hex digits then '_'.
		unsigned int v = 0;
		for (int i = 0; i < 8; ++i) {
			char c = filename[i];
			unsigned int d;
			if (c >= '0' && c <= '9')      d = c - '0';
			else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
			else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
			else                            return 0;
			v = (v << 4) | d;
		}
		if (filename[8] != '_') return 0;
		return v & 0xFFFFFF;  // mask to local formID
	}

	// Parse race name from filename of form "<RaceName>_<Component>.<ext>".
	// Returns lowercased race name or "" if filename doesn't match.
	static std::string ParseRaceNameFromFilename(const char* filename)
	{
		if (!filename) return "";
		const char* underscore = strchr(filename, '_');
		if (!underscore || underscore == filename) return "";
		std::string name(filename, underscore - filename);
		for (auto& c : name) c = (char)std::tolower((unsigned char)c);
		return name;
	}

	// Walk a directory recursively. For each regular file, call the callback with the
	// filename (not the full path). Quietly returns if root doesn't exist.
	template <typename Callback>
	static void WalkFiles(const std::string& root, Callback cb)
	{
		std::string search = root + "\\*";
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA(search.c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return;

		do {
			if (fd.cFileName[0] == '.') continue;
			std::string child = root + "\\" + fd.cFileName;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				WalkFiles(child, cb);
			} else {
				cb(fd.cFileName);
			}
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}

	// Collect every formID we can parse from filenames in any subdir of `root`.
	static void CollectFormIDs(const std::string& root, std::unordered_set<unsigned int>& out)
	{
		WalkFiles(root, [&](const char* name) {
			unsigned int id = ParseFormIDFromFilename(name);
			if (id) out.insert(id);
		});
	}

	// Collect every race name we can parse from filenames in any subdir of `root`.
	static void CollectRaceNames(const std::string& root, std::unordered_set<std::string>& out)
	{
		WalkFiles(root, [&](const char* name) {
			std::string r = ParseRaceNameFromFilename(name);
			if (!r.empty()) out.insert(r);
		});
	}

	// Walk the immediate subdirectories of `root` (one level only). For each, check whether
	// its file tree contains any file with `_M.<ext>` or `_F.<ext>` suffix. If so, add the
	// subdir name (lowercased) to `out`. Used to find races that have PerRace head
	// gender-variant override files inline with their race meshes.
	static void CollectRacesWithGenderVariants(const std::string& root, std::unordered_set<std::string>& out)
	{
		std::string search = root + "\\*";
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA(search.c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return;

		do {
			if (fd.cFileName[0] == '.') continue;
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
			std::string raceDir = root + "\\" + fd.cFileName;
			bool hasVariant = false;
			WalkFiles(raceDir, [&](const char* fname) {
				if (hasVariant) return;
				size_t len = strlen(fname);
				// Match "*_M.nif", "*_F.nif", "*_M.dds", "*_F.dds" (case-insensitive .ext)
				if (len < 6) return;
				if (fname[len-6] != '_') return;
				char gender = (char)std::tolower((unsigned char)fname[len-5]);
				if (gender != 'm' && gender != 'f') return;
				if (fname[len-4] != '.') return;
				hasVariant = true;
			});
			if (hasVariant) {
				std::string n = ToLower(fd.cFileName);
				out.insert(n);
			}
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}

	void ScanAtStartup()
	{
		// Head: PerNPC files at Meshes\Characters\HeadAssetOverrides\PerNPC\<plugin>\<formID>_<comp>.nif
		//       PerRace files at Meshes\Characters\HeadAssetOverrides\PerRace\<gender>\<race>_<comp>.nif
		// Mirror under Textures\... for .dds files.
		CollectFormIDs("Data\\Meshes\\Characters\\HeadAssetOverrides\\PerNPC",   s_HeadPerNPC);
		CollectFormIDs("Data\\Textures\\Characters\\HeadAssetOverrides\\PerNPC", s_HeadPerNPC);
		CollectRaceNames("Data\\Meshes\\Characters\\HeadAssetOverrides\\PerRace",   s_HeadPerRace);
		CollectRaceNames("Data\\Textures\\Characters\\HeadAssetOverrides\\PerRace", s_HeadPerRace);

		CollectFormIDs("Data\\Meshes\\Characters\\BodyAssetOverrides\\PerNPC",   s_BodyPerNPC);
		CollectFormIDs("Data\\Textures\\Characters\\BodyAssetOverrides\\PerNPC", s_BodyPerNPC);
		CollectRaceNames("Data\\Meshes\\Characters\\BodyAssetOverrides\\PerRace",   s_BodyPerRace);
		CollectRaceNames("Data\\Textures\\Characters\\BodyAssetOverrides\\PerRace", s_BodyPerRace);

		// Gender-variant head overrides live inline with race meshes:
		//   Meshes\Characters\<race>\<asset>_M.nif   (or _F)
		//   Textures\Characters\<race>\<asset>_M.dds (or _F)
		CollectRacesWithGenderVariants("Data\\Meshes\\Characters",   s_HeadGenderVariantRaces);
		CollectRacesWithGenderVariants("Data\\Textures\\Characters", s_HeadGenderVariantRaces);

		_MESSAGE("[RBRN] FastPath scan: HeadPerNPC=%zu HeadPerRace=%zu HeadVariantRaces=%zu BodyPerNPC=%zu BodyPerRace=%zu",
			s_HeadPerNPC.size(), s_HeadPerRace.size(), s_HeadGenderVariantRaces.size(),
			s_BodyPerNPC.size(), s_BodyPerRace.size());
	}

	bool HeadHasPerNPC(unsigned int npcRefID)
	{
		return s_HeadPerNPC.count(npcRefID & 0xFFFFFF) != 0;
	}

	bool HeadHasPerRace(const char* raceName)
	{
		if (!raceName) return false;
		return s_HeadPerRace.count(ToLower(raceName)) != 0;
	}

	bool HeadRaceHasGenderVariants(const char* raceName)
	{
		if (!raceName) return false;
		return s_HeadGenderVariantRaces.count(ToLower(raceName)) != 0;
	}

	bool BodyHasPerNPC(unsigned int npcRefID)
	{
		return s_BodyPerNPC.count(npcRefID & 0xFFFFFF) != 0;
	}

	bool BodyHasPerRace(const char* raceName)
	{
		if (!raceName) return false;
		return s_BodyPerRace.count(ToLower(raceName)) != 0;
	}
}
