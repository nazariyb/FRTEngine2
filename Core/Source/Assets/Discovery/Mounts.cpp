#include "Assets/Discovery/Mounts.h"

#include <algorithm>
#include <utility>

#include "EnginePaths.h"
#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Discovery/BuiltinExcludes.h"
#include "Assets/Discovery/MountScanner.h"


namespace frt::assets
{
namespace
{
constexpr FStringLiteral EngineMountName = "Engine";
constexpr FStringLiteral ProjectMountName = "Project";
constexpr FStringLiteral PluginMountPrefix = "Plugin.";

// The layout below a project (§3.1): its content, its rule file, its plugins.
constexpr FStringLiteral ContentDirName = "Content";
constexpr FStringLiteral ConfigDirName = "Config";
constexpr FStringLiteral PluginsDirName = "Plugins";

void AddWarning (std::vector<SDiagnostic>* Out, uint32 Code, FStringView Subject, FString Message)
{
	if (Out == nullptr)
	{
		return;
	}
	SDiagnostic diagnostic;
	diagnostic.Severity = ESeverity::Warning;
	diagnostic.Code = Code;
	diagnostic.Subject = FString(Subject);
	diagnostic.Message = std::move(Message);
	Out->push_back(std::move(diagnostic));
}

bool IsDirectory (const std::filesystem::path& Path)
{
	const std::optional<platform::SFileStat> stat = platform::Stat(Path);
	return stat.has_value() && stat->bDirectory;
}

// Fills the volume facts of the mount root; a missing root keeps the defaults (case-insensitive), and a
// warning says the mount is empty.
SMount MakeMount (FString Name, EMountKind Kind, const std::filesystem::path& ContentDir, const std::filesystem::path& RulesFile,
	const std::filesystem::path& OwnerRoot, std::vector<SDiagnostic>* Out)
{
	SMount mount;
	mount.Name = std::move(Name);
	mount.Kind = Kind;
	mount.ContentDir = platform::NormalizePath(ContentDir);
	mount.RulesFile = platform::NormalizePath(RulesFile);
	mount.OwnerRoot = platform::NormalizePath(OwnerRoot);
	if (IsDirectory(mount.ContentDir))
	{
		FString error;
		if (std::optional<platform::SVolumeInfo> volume = platform::QueryVolumeInfo(mount.ContentDir, &error))
		{
			mount.Volume = std::move(*volume);
		}
		else
		{
			AddWarning(Out, 0u, mount.GetRootMountPath(), FString::Format("cannot query the volume of {}: {}; assuming a case-insensitive "
				"file system", FString::FromPath(mount.ContentDir), error));
		}
	}
	else if (Kind != EMountKind::Plugin)
	{
		AddWarning(Out, 0u, mount.GetRootMountPath(), FString::Format("the content directory {} does not exist: the mount is empty",
			FString::FromPath(mount.ContentDir)));
	}
	return mount;
}

// The deepest directory at or above both (they are normalized). Falls back to the root of From.
std::filesystem::path FindCommonDirectory (const std::filesystem::path& From, const std::filesystem::path& Other)
{
	std::filesystem::path candidate = From;
	while (!platform::IsAtOrBelow(Other, candidate))
	{
		const std::filesystem::path parent = candidate.parent_path();
		if (parent.empty() || parent == candidate)
		{
			return candidate;
		}
		candidate = parent;
	}
	return candidate;
}

// The plugin folders below <project>/Plugins that have a Content/ directory, in MountId order.
std::vector<FString> FindPlugins (const std::filesystem::path& PluginsDir, std::vector<SDiagnostic>* Out)
{
	std::vector<FString> names;
	if (!IsDirectory(PluginsDir))
	{
		return names;
	}
	FString error;
	std::vector<FString> candidates;
	const bool bListed = platform::EnumerateDirectory(PluginsDir, [&candidates] (const platform::SDirectoryEntry& Entry)
	{
		if (Entry.Stat.bDirectory && !IsBuiltinExcludedName(Entry.Name, true))
		{
			candidates.emplace_back(Entry.Name);
		}
	}, &error);
	if (!bListed)
	{
		AddWarning(Out, 0u, JoinMountPath(ProjectMountName, FStringView()), FString::Format("cannot list the plugins: {}", error));
		return names;
	}

	for (FString& name : candidates)
	{
		// The name first: a name Win32 rewrites ("Foo.", "aux") would have its Content looked up as another
		// folder's, so it is reported whether or not it looks like a plugin.
		const FStringView reason = GetNonPortableNameReason(name);
		if (!reason.IsEmpty())
		{
			const FString shown = ToDisplayText(name.View());
			AddWarning(Out, Path002, JoinMountPath(FString(PluginMountPrefix) + shown, FStringView()),
				FString::Format("the plugin folder name '{}' is not portable: it {}; the folder is not mounted: rename it in {}", shown, reason,
					FString::FromPath(PluginsDir)));
			continue;
		}
		if (!IsDirectory(PluginsDir / name.ToPath() / ContentDirName.ToPath()))
		{
			continue;
		}
		names.push_back(std::move(name));
	}
	std::sort(names.begin(), names.end(), [] (const FString& A, const FString& B)
	{
		const int32 folded = A.View().Compare<ESearchCase::IgnoreCase>(B.View());
		return folded != 0 ? folded < 0 : A < B;
	});

	// Plugins equal under the portable fold (Foo and foo in a case-sensitive Plugins folder, U+212A KELVIN
	// SIGN and 'K' anywhere) are both mounted, but Find by a typed name reaches only the first and their
	// mount paths collide wherever lookups fold: PATH001 for each, as for sibling files. Adjacent after the
	// sort.
	for (size_t start = 0u; start < names.size();)
	{
		size_t end = start + 1u;
		while (end < names.size() && names[end].View().Equals<ESearchCase::IgnoreCase>(names[start].View()))
		{
			++end;
		}
		for (size_t k = start; end - start > 1u && k < end; ++k)
		{
			FString others;
			for (size_t o = start; o < end; ++o)
			{
				if (o != k)
				{
					others.AppendFormat("{}'{}'", others.IsEmpty() ? "" : ", ", names[o]);
				}
			}
			AddWarning(Out, Path001, JoinMountPath(FString(PluginMountPrefix) + names[k], FStringView()), FString::Format("the plugin "
				"folders '{}' and {} differ only in case: both are mounted, but a mount name typed in a query or rule file finds only "
				"the first, and their mount paths collide wherever lookups fold case", names[k], others));
		}
		start = end;
	}
	return names;
}
}


FString ToDisplayText (FStringView OnDiskSpelling)
{
	if (OnDiskSpelling.IsValidUtf8())
	{
		return FString(OnDiskSpelling);
	}
	// A surrogate code point in WTF-8 is ED A0..BF 80..BF; strict UTF-8 decoding would show it as three
	// U+FFFD (one per maximal subpart), the name has one unit there.
	FString text;
	text.Reserve(OnDiskSpelling.Len() + 8u);
	const uint32 length = OnDiskSpelling.Len();
	for (uint32 i = 0u; i < length;)
	{
		const auto byte = [&OnDiskSpelling] (uint32 Index) { return static_cast<uint8>(OnDiskSpelling[Index]); };
		if (byte(i) == 0xEDu && i + 2u < length && byte(i + 1u) >= 0xA0u && byte(i + 1u) <= 0xBFu && byte(i + 2u) >= 0x80u && byte(i + 2u) <= 0xBFu)
		{
			text.AppendCodePoint(U'\xFFFD');
			i += 3u;
			continue;
		}
		text.AppendChar(OnDiskSpelling[i]);
		++i;
	}
	return text.View().IsValidUtf8() ? text : FString::FromUtf8Lossy(text.View());
}


CMountTable CMountTable::Resolve (const SMountRoots& Roots, std::vector<SDiagnostic>* Out)
{
	CMountTable table;
	SMount engine = MakeMount(FString(EngineMountName), EMountKind::Engine, Roots.EngineContentDir, Roots.EngineConfigDir / RuleFileName.ToPath(),
		Roots.EngineDir, Out);
	engine.Id = EngineMountId;
	table.Mounts.push_back(std::move(engine));

	if (Roots.ProjectDir.empty())
	{
		return table;
	}
	const std::filesystem::path projectDir = platform::NormalizePath(Roots.ProjectDir);
	SMount project = MakeMount(FString(ProjectMountName), EMountKind::Project, projectDir / ContentDirName.ToPath(),
		projectDir / ConfigDirName.ToPath() / RuleFileName.ToPath(), projectDir, Out);
	project.Id = ProjectMountId;
	table.Mounts.push_back(std::move(project));

	const std::filesystem::path pluginsDir = projectDir / PluginsDirName.ToPath();
	for (const FString& name : FindPlugins(pluginsDir, Out))
	{
		const std::filesystem::path pluginDir = pluginsDir / name.ToPath();
		SMount plugin = MakeMount(FString(PluginMountPrefix) + name, EMountKind::Plugin, pluginDir / ContentDirName.ToPath(),
			pluginDir / ConfigDirName.ToPath() / RuleFileName.ToPath(), projectDir, Out);
		plugin.Id = static_cast<MountId>(table.Mounts.size());
		table.Mounts.push_back(std::move(plugin));
	}
	return table;
}

CMountTable CMountTable::Resolve (const std::filesystem::path& EngineContentDir, const std::filesystem::path& EngineConfigDir,
	const std::filesystem::path& ProjectDir, std::vector<SDiagnostic>* Out)
{
	SMountRoots roots;
	roots.EngineContentDir = platform::NormalizePath(EngineContentDir);
	roots.EngineConfigDir = platform::NormalizePath(EngineConfigDir);
	roots.ProjectDir = ProjectDir;
	const std::filesystem::path& engineDir = paths::GetEngineDir();
	if (!engineDir.empty() && platform::IsAtOrBelow(roots.EngineContentDir, platform::NormalizePath(engineDir))
		&& platform::IsAtOrBelow(roots.EngineConfigDir, platform::NormalizePath(engineDir)))
	{
		roots.EngineDir = engineDir;
	}
	else
	{
		roots.EngineDir = FindCommonDirectory(roots.EngineConfigDir, roots.EngineContentDir);
	}
	return Resolve(roots, Out);
}

const SMount* CMountTable::Find (FStringView Name) const noexcept
{
	const SMount* folded = nullptr;
	for (const SMount& mount : Mounts)
	{
		if (mount.Name.View() == Name)
		{
			return &mount;
		}
		if (folded == nullptr && mount.Name.View().Equals<ESearchCase::IgnoreCase>(Name))
		{
			folded = &mount;
		}
	}
	return folded;
}

const SMount* CMountTable::Find (MountId Id) const noexcept
{
	return Id < Mounts.size() ? &Mounts[Id] : nullptr;
}

const SMount* CMountTable::FindContaining (const std::filesystem::path& Absolute) const
{
	const std::filesystem::path path = platform::NormalizePath(Absolute);
	const SMount* best = nullptr;
	size_t bestLength = 0u;
	bool bBestExact = false;
	for (const SMount& mount : Mounts)
	{
		// The root prefix by NTFS's rule, whatever the mount's own: the directories that hold a mount root
		// (and the root's own name) are looked up by their parents' rule, not by the root's. Mounts are
		// disjoint by layout, but a project inside the engine checkout (Templates/) nests directories: the
		// longest root wins, and between roots equal under the rule (plugins Foo and foo in a
		// case-sensitive Plugins folder) the byte-exact one.
		if (!platform::IsAtOrBelow(path, mount.ContentDir))
		{
			continue;
		}
		const size_t length = mount.ContentDir.native().size();
		const bool bExact = platform::IsAtOrBelow(path, mount.ContentDir, true);
		if (best == nullptr || length > bestLength || (length == bestLength && bExact && !bBestExact))
		{
			best = &mount;
			bestLength = length;
			bBestExact = bExact;
		}
	}
	return best;
}

std::optional<FString> CMountTable::ToMountPath (const std::filesystem::path& Absolute) const
{
	const std::filesystem::path path = platform::NormalizePath(Absolute);
	const SMount* mount = FindContaining(path);
	if (mount == nullptr)
	{
		return std::nullopt;
	}
	const std::optional<FString> relative = platform::GetRelativePath(path, mount->ContentDir);
	if (!relative.has_value())
	{
		return std::nullopt;
	}
	return JoinMountPath(mount->Name, relative->View());
}

std::optional<std::filesystem::path> CMountTable::FromMountPath (FStringView MountPath) const
{
	FStringView mountName;
	FStringView relative;
	if (!IsWellFormedMountPath(MountPath) || !SplitMountPath(MountPath, mountName, relative))
	{
		return std::nullopt;
	}
	const SMount* mount = Find(mountName);
	if (mount == nullptr)
	{
		return std::nullopt;
	}
	if (relative.IsEmpty())
	{
		return mount->ContentDir;
	}
	if (relative.Contains('\\') || relative.Contains(':') || relative.Contains('\0'))
	{
		return std::nullopt;
	}
	std::optional<std::filesystem::path> below = relative.TryToPath();
	if (!below.has_value())
	{
		return std::nullopt;
	}
	std::filesystem::path path = mount->ContentDir / *below;
	path.make_preferred();
	return path;
}
}
