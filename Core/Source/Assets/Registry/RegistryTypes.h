#pragma once

#include <bit>
#include <concepts>
#include <type_traits>

#include "CoreTypes.h"
#include "Enum.h"
#include "Assets/AssetId.h"
#include "Containers/String.h"


/**
 * The vocabulary of the asset registry (design §5.2, §5.7): the enums and small value types every registry
 * piece - the store, discovery, the rules engine, the importers' Analyze, queries, snapshots - compiles
 * against. Header-only and dependency-light on purpose: workers include it to fill deltas (RegistryDelta.h)
 * and scan batches (Discovery/ScanTypes.h), so nothing here touches the engine's memory pool.
 *
 * The enums are copied verbatim from the spec; their values are part of the snapshot format (§5.3) once
 * snapshots exist, so a value is appended, never renumbered.
 */
namespace frt::reflect
{
// A runtime type index. M3 numbers the asset types by hand (Assets/AssetTypes.h); M5's reflection keeps the
// name and replaces only this definition. Never written to disk as such: snapshots store type names and
// remap them at load (§5.2, §5.3).
using TypeId = uint16;
inline constexpr TypeId InvalidTypeId = 0xFFFFu;
}


namespace frt::assets
{
/**
 * A mount's index in this process: 0 = /Engine, 1 = /Project, then /Plugin.<Name> from 2 in
 * SStringLessIgnoreCase order of Name (ties broken by byte order). Assigned at each start-up, never
 * meaningful across processes: snapshots persist mounts by name and remap ids at load.
 */
using MountId = uint16;
inline constexpr MountId EngineMountId = 0u;
inline constexpr MountId ProjectMountId = 1u;
inline constexpr MountId FirstPluginMountId = 2u;
inline constexpr MountId InvalidMountId = 0xFFFFu;


// ---------------- §5.2 ----------------

// Valid only inside one registry view (§5.9). Cross-frame code keeps an SAssetId and re-resolves.
struct SRecordId
{
	uint32 Value = ~0u; uint32 Revision = 0; constexpr bool IsSubEntry () const { return (Value >> 31u) != 0u; }

	// ~0u (the default) names no record. It has the sub-entry bit set: test IsValid first.
	[[nodiscard]] constexpr bool IsValid () const noexcept { return Value != ~0u; }
	constexpr bool operator== (const SRecordId&) const noexcept = default;
};

enum class EFileClass : uint8 { Asset, Companion, Raw, Unclaimed, Excluded };
enum class EMetaState : uint8 { Paired, PendingMeta, Orphan, Broken, MissingMeta };   // M3: always PendingMeta
enum class ERecordFlags : uint16
{
	None = 0, Primary = 1 << 0, HasProblems = 1 << 1, StaleTags = 1 << 2, LoadedThroughRedirect = 1 << 3,
	Unsaved = 1 << 4 /* editor overlay */, EditorOnly = 1 << 5, PinnedId = 1 << 6, Schemaless = 1 << 7,
	LegacyReferences = 1 << 8, Aliased = 1 << 9, TypeExcluded = 1 << 10
};
FRT_DECLARE_FLAG_ENUM(ERecordFlags)

enum class ETagType : uint8 { None, Int64, Double, Bool, Name, String, Enum, AssetRef, RowRef };
enum class ETagFlags : uint16
{
	None = 0, CookToRuntime = 1 << 1, MultiValue = 1 << 2, Hidden = 1 << 3,
	AfterBuild = 1 << 4 /* known only once built: MeanColor, VertexCount */, EditorOnly = 1 << 5
};
FRT_DECLARE_FLAG_ENUM(ETagFlags)

enum class EDepFlags : uint16
{
	Hard           = 1 << 0,   // owner is not Ready until the target is Ready (absent = Soft)
	EditorOnly     = 1 << 1,   // never followed by the cook (UE: not Game)
	Build          = 1 << 2,   // read by import/build: companions, upstream outputs, shader includes, path resolutions
	Row            = 1 << 3,   // TargetSub is a row id
	Unresolved     = 1 << 4,   // target not (yet) registered; resolves when it appears
	Declared       = 1 << 5,   // typed field or importer declaration (absent = recorded build read)
	Bundle         = 1 << 6,   // soft edge that belongs to at least one bundle (§10.3)
	Schemaless     = 1 << 7,   // found by the $asset scan without the type: kind unknown, the cook treats it as Hard
	Path           = 1 << 8,   // the source names the target by path (glTF URI, legacy reference): a move must warn
	Code           = 1 << 9,   // found by the source scanner in game code (FRT_ASSET)
};
FRT_DECLARE_FLAG_ENUM(EDepFlags)


// Bitwise operators for the three registry flag enums (Enum.h's SFlags wrapper suits mutable flag fields;
// these suit the plain enum members of deltas and records: `EDepFlags::Build | EDepFlags::Path`).
template <typename E>
concept RegistryFlagEnum = std::same_as<E, ERecordFlags> || std::same_as<E, ETagFlags> || std::same_as<E, EDepFlags>;

template <RegistryFlagEnum E>
[[nodiscard]] constexpr E operator| (E A, E B) noexcept
{
	using U = std::underlying_type_t<E>;
	return static_cast<E>(static_cast<U>(static_cast<U>(A) | static_cast<U>(B)));
}

template <RegistryFlagEnum E>
[[nodiscard]] constexpr E operator& (E A, E B) noexcept
{
	using U = std::underlying_type_t<E>;
	return static_cast<E>(static_cast<U>(static_cast<U>(A) & static_cast<U>(B)));
}

template <RegistryFlagEnum E>
[[nodiscard]] constexpr E operator~ (E A) noexcept
{
	using U = std::underlying_type_t<E>;
	return static_cast<E>(static_cast<U>(~static_cast<U>(A)));
}

template <RegistryFlagEnum E>
constexpr E& operator|= (E& A, E B) noexcept
{
	return A = A | B;
}

template <RegistryFlagEnum E>
constexpr E& operator&= (E& A, E B) noexcept
{
	return A = A & B;
}

// Any bit of Mask set in Flags (false for an empty Mask).
template <RegistryFlagEnum E>
[[nodiscard]] constexpr bool HasAnyFlags (E Flags, E Mask) noexcept
{
	return static_cast<std::underlying_type_t<E>>(Flags & Mask) != 0u;
}

// Every bit of Mask set in Flags (true for an empty Mask).
template <RegistryFlagEnum E>
[[nodiscard]] constexpr bool HasAllFlags (E Flags, E Mask) noexcept
{
	return (Flags & Mask) == Mask;
}


/**
 * Fixed for M3 where §5.2 names a column but not its values. ECookRule is the rule files' `Cook:` value
 * (`Auto` there means Default: the cook decides by reachability and mode, §10). EImportStatus says how far
 * the importer got with the file: Analyze ran and succeeded, failed (the record is kept, with a diagnostic),
 * or the file's importer is known but not available in this build or on this machine (§3.5.3, IMP010).
 */
enum class ECookRule : uint8 { Default, Always, Never, EditorOnly, DevelopmentOnly };
enum class EImportStatus : uint8 { NotAnalyzed, Analyzed, AnalyzeFailed, ImporterUnavailable };


// ---------------- §5.7 ----------------

enum class ECompare : uint8 { Eq, Ne, Lt, Le, Gt, Ge, In, Prefix, Contains, Exists, Missing };
enum class EExists : uint8 { No, Yes, Unknown };                   // Unknown only while a scan is incomplete (tools)
enum class ERecordScope : uint8 { Assets, Rows, SubAssets, All };

/**
 * A tag value as code outside the store passes it (deltas, queries, GetTag results). Bits holds Int64 (two's
 * complement), Double (IEEE bits), Bool (0/1), Enum (the value) and AssetRef/RowRef (an SAssetId64 / row key);
 * Name and String carry their text in Text (the store interns it; NameId and string offsets never leave a
 * view). Aux is per type: the enum table or row-table index inside a view, 0 elsewhere.
 */
struct STagValue
{
	ETagType Type = ETagType::None; uint32 Aux = 0; uint64 Bits = 0; FString Text;

	[[nodiscard]] static STagValue FromInt64 (int64 Number) noexcept
	{
		STagValue value;
		value.Type = ETagType::Int64;
		value.Bits = static_cast<uint64>(Number);
		return value;
	}

	[[nodiscard]] static STagValue FromDouble (double Number) noexcept
	{
		STagValue value;
		value.Type = ETagType::Double;
		value.Bits = std::bit_cast<uint64>(Number);
		return value;
	}

	[[nodiscard]] static STagValue FromBool (bool bValue) noexcept
	{
		STagValue value;
		value.Type = ETagType::Bool;
		value.Bits = bValue ? 1u : 0u;
		return value;
	}

	[[nodiscard]] static STagValue FromName (FStringView Name) noexcept
	{
		STagValue value;
		value.Type = ETagType::Name;
		value.Text = FString(Name);
		return value;
	}

	[[nodiscard]] static STagValue FromString (FStringView String) noexcept
	{
		STagValue value;
		value.Type = ETagType::String;
		value.Text = FString(String);
		return value;
	}

	[[nodiscard]] int64 AsInt64 () const noexcept { return static_cast<int64>(Bits); }
	[[nodiscard]] double AsDouble () const noexcept { return std::bit_cast<double>(Bits); }
	[[nodiscard]] bool AsBool () const noexcept { return Bits != 0u; }

	bool operator== (const STagValue&) const = default;
};


// ---------------- Identity in M3 ----------------

/**
 * The provisional id of a file's record while it has no .meta (all of M3; M4 writes metas):
 * MakeDerivedGuid(SGuid{}, "path", MountPath), MountPath as spelled on disk. The only place it is computed.
 * It is a registry-internal key: never written to a content file, never handed to runtime code. Two paths
 * that differ only in case (PATH001) get different ids.
 */
[[nodiscard]] inline SGuid MakeProvisionalGuid (FStringView MountPath) noexcept
{
	return MakeDerivedGuid(SGuid {}, "path", MountPath);
}


// ---------------- Mount paths ----------------
//
// A mount path is "/<MountName>/<relative>": MountName is "Engine", "Project" or "Plugin.<Name>"; the
// relative part is in on-disk spelling (canonical WTF-8 names from EnumerateDirectory), '/' separated. A
// mount root is "/<MountName>" (no trailing '/'). A "#label" suffix (a sub-entry) is the callers' business:
// these helpers see it as part of the leaf. They are pure text functions: no fold, no disk access.

/**
 * "/Project/A/b.png" -> "Project", "A/b.png"; "/Project" -> "Project", "". False (outputs untouched) when
 * Path does not start with '/' or names no mount ("", "/", "//x"). Nothing else is checked: use
 * IsWellFormedMountPath for that. The outputs view Path.
 */
[[nodiscard]] inline bool SplitMountPath (FStringView Path, FStringView& OutMountName, FStringView& OutRelative) noexcept
{
	if (Path.IsEmpty() || Path[0] != '/')
	{
		return false;
	}
	const SStringSplit split = Path.RightChop(1u).SplitFirst('/');
	if (split.Left.IsEmpty())
	{
		return false;
	}
	OutMountName = split.Left;
	OutRelative = split.Right;
	return true;
}

// The inverse of SplitMountPath: "/" + MountName, then "/" + Relative when Relative is not empty.
[[nodiscard]] inline FString JoinMountPath (FStringView MountName, FStringView Relative) noexcept
{
	FString path;
	path.Reserve(2u + MountName.Len() + Relative.Len());
	path += '/';
	path += MountName;
	if (!Relative.IsEmpty())
	{
		path += '/';
		path += Relative;
	}
	return path;
}

// ParentMountPath + "/" + Child (a name, or a relative path below the parent): the path of an entry found
// while listing a folder. ParentMountPath is a mount path (a mount root included).
[[nodiscard]] inline FString MakeChildMountPath (FStringView ParentMountPath, FStringView Child) noexcept
{
	FString path;
	path.Reserve(ParentMountPath.Len() + 1u + Child.Len());
	path += ParentMountPath;
	path += '/';
	path += Child;
	return path;
}

// A leading '/', no trailing '/', and no empty, "." or ".." segment ("/Project" and "/Project/A/b.png" are
// well-formed; "", "/", "Project/A", "/Project/", "/Project//A", "/Project/./A" and "/Project/../A" are not).
[[nodiscard]] inline bool IsWellFormedMountPath (FStringView Path) noexcept
{
	if (Path.Len() < 2u || Path[0] != '/')
	{
		return false;
	}
	FStringView rest = Path.RightChop(1u);
	for (;;)
	{
		const SStringSplit split = rest.SplitFirst('/');
		if (split.Left.IsEmpty() || split.Left == "." || split.Left == "..")
		{
			return false;
		}
		if (!split.bFound)
		{
			return true;
		}
		rest = split.Right;
	}
}

// "/Project/A/b.png" -> "/Project/A"; "/Project/A" -> "/Project"; a mount root ("/Project") or anything
// without a '/' after the first character -> "" (no parent). Views Path.
[[nodiscard]] inline FStringView GetMountPathParent (FStringView Path) noexcept
{
	if (Path.Len() < 2u)
	{
		return FStringView();
	}
	const SStringSplit split = Path.RightChop(1u).SplitLast('/');
	if (!split.bFound)
	{
		return FStringView();
	}
	return Path.Left(1u + split.Left.Len());
}

// The last segment: "/Project/A/b.png" -> "b.png"; a mount root "/Project" -> "Project". Views Path.
[[nodiscard]] inline FStringView GetMountPathLeaf (FStringView Path) noexcept
{
	const SStringSplit split = Path.SplitLast('/');
	return split.bFound ? split.Right : Path;
}
}
