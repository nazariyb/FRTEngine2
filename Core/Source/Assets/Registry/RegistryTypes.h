#pragma once

#include <bit>
#include <concepts>
#include <optional>
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
 * snapshots exist, so a value is appended, never renumbered. Every enum has text names (Enum.h's
 * enum_::ToString / TryParse, declared at the end of this file): the names are the C++ identifiers, and
 * every piece that prints or parses one - the dump, the query syntax, explain, status, the rules engine -
 * uses them, so the spellings never drift. The flag enums have one name per bit (no None entry): print a
 * combination bit by bit.
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

// Meta (M4): a `.meta` file's own FILES row, so that it has a stat and an identity and an orphan has a home.
enum class EFileClass : uint8 { Asset, Companion, Raw, Unclaimed, Excluded, Meta };
// §4.7's ErrorMissingMeta is PendingMeta plus an error-severity META001; MissingMeta is a lost meta (META008).
enum class EMetaState : uint8 { Paired, PendingMeta, Orphan, Broken, MissingMeta };   // until M4's lifecycle: always PendingMeta
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
 * Fixed for M3 where §5.2 names a column but not its values. ECookRule is the rule files' `Cook:` value,
 * spelled as they spell it (Auto: the cook decides by reachability and mode, §10), so rule files, explain,
 * list-rules, the dump and queries share one name per value through the enum reflection. EImportStatus says how far
 * the importer got with the file: Analyze ran and succeeded, failed (the record is kept, with a diagnostic),
 * or the file's importer is known but not available in this build or on this machine (§3.5.3, IMP010).
 */
enum class ECookRule : uint8 { Auto, Always, Never, EditorOnly, DevelopmentOnly };
enum class EImportStatus : uint8 { NotAnalyzed, Analyzed, AnalyzeFailed, ImporterUnavailable };


// ---------------- §5.7 ----------------

/**
 * §5.7's operators plus Match (appended; queries are never persisted in this form). Match is the text
 * syntax's ':' (§5.8), which the parser lowers without knowing key types: Compile turns it into Prefix for
 * text keys (Name, String) and Eq for every other type. Builder callers that know the key use Prefix or Eq.
 * In takes its set as several values, one STagValue each (SAssetQuery's WhereIn), never a list packed
 * into Text.
 */
enum class ECompare : uint8 { Eq, Ne, Lt, Le, Gt, Ge, In, Prefix, Contains, Exists, Missing, Match };
enum class EExists : uint8 { No, Yes, Unknown };                   // Unknown only while a scan is incomplete (tools)
enum class ERecordScope : uint8 { Assets, Rows, SubAssets, All };

/**
 * A tag value as code outside the store passes it (deltas, queries, GetTag results). Bits holds Int64 (two's
 * complement), Double (IEEE bits), Bool (0/1), Enum (the value) and AssetRef/RowRef (an SAssetId64 / row key);
 * Name and String carry their text in Text (the store interns it; NameId and string offsets never leave a
 * view). Aux is per type: the enum table or row-table index inside a view, 0 elsewhere.
 *
 * Type None with Text set is a query literal as written, not yet typed (FromLiteral): the text syntax
 * cannot know whether `Width>=4096` compares an Int64 or a Double, so Compile converts the literal per key
 * type (enum names, numbers, bools, names; §5.6). Its Aux holds AuxQuoted when the literal was quoted, so
 * `'1'` stays text and never becomes a number. Deltas never carry an untyped value.
 */
struct STagValue
{
	ETagType Type = ETagType::None; uint32 Aux = 0; uint64 Bits = 0; FString Text;

	static constexpr uint32 AuxQuoted = 1u << 0u;   // Type None only: the literal was quoted

	[[nodiscard]] static STagValue FromLiteral (FStringView Text, bool bQuoted) noexcept
	{
		STagValue value;
		value.Aux = bQuoted ? AuxQuoted : 0u;
		value.Text = FString(Text);
		return value;
	}

	// An untyped query literal (FromLiteral) that Compile has not converted yet. A quoted empty literal ('')
	// counts; an unquoted one cannot be written.
	[[nodiscard]] bool IsLiteral () const noexcept { return Type == ETagType::None && (!Text.IsEmpty() || (Aux & AuxQuoted) != 0u); }
	[[nodiscard]] bool IsQuotedLiteral () const noexcept { return Type == ETagType::None && (Aux & AuxQuoted) != 0u; }

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
// mount root is "/<MountName>" (no trailing '/'). These helpers are pure text functions: no fold, no disk
// access.
//
// They take a file or folder mount path only, never one with a "#label" suffix (a sub-entry). A label may
// contain '/' (glTF names do: "mesh:Armature/Body") and '#' ("image:#0", the "#2" duplicate suffix), so
// SplitMountPath, IsWellFormedMountPath, GetMountPathParent, GetMountPathLeaf and ResolveMountPath would
// split inside it. A caller splits the label off first, at the '#' that follows the file's mount path -
// which it knows from the record, never by searching for '#', because '#' is legal in file names.

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

/**
 * Path is Ancestor or lies below it, byte-exact (mount paths are keyed by their on-disk spelling): Ancestor itself, or
 * Ancestor followed by '/' and more. bLabels: Ancestor followed by '#' counts too - a sub-entry "<file>#<label>" of the
 * file Ancestor, the Subject of its problems.
 */
[[nodiscard]] inline bool IsMountPathAtOrBelow (FStringView Path, FStringView Ancestor, bool bLabels = false) noexcept
{
	if (Path.Len() <= Ancestor.Len())
	{
		return Path == Ancestor;
	}
	const char next = Path[Ancestor.Len()];
	return (next == '/' || (bLabels && next == '#')) && Path.StartsWith(Ancestor);
}

/**
 * The registry's deterministic order of mount paths and names: the portable fold (FString::FoldCase), ties broken by
 * byte order - a scan batch's order (ScanTypes.h), FOLDERS' sibling order, the dump's and Rules.cache's order (§5.3).
 */
[[nodiscard]] inline bool LessPortableThenBytes (FStringView A, FStringView B) noexcept
{
	const int32 folded = A.Compare<ESearchCase::IgnoreCase>(B);
	return folded != 0 ? folded < 0 : A < B;
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

/**
 * The mount a mount path, or a problem subject, belongs to: its first segment, which ends at the first '/' or '#'
 * ("/Project#x" names /Project; mount names never hold '#'). Empty when Path names no mount. Views Path.
 */
[[nodiscard]] inline FStringView GetMountNameOf (FStringView Path) noexcept
{
	FStringView mountName;
	FStringView relative;
	if (!SplitMountPath(Path, mountName, relative))
	{
		return FStringView();
	}
	const uint32 hash = mountName.Find('#');
	return hash == FStringView::NotFound ? mountName : mountName.Left(hash);
}

// The last segment: "/Project/A/b.png" -> "b.png"; a mount root "/Project" -> "Project". Views Path.
[[nodiscard]] inline FStringView GetMountPathLeaf (FStringView Path) noexcept
{
	const SStringSplit split = Path.SplitLast('/');
	return split.bFound ? split.Right : Path;
}

/**
 * The mount path a reference names, in the one canonical form that deltas carry (SEdgeDecl::TargetPath,
 * SRecordUpsert::Companions) and queries resolve (ref:/dep: targets). A Reference led by '/' or '\' is
 * absolute: its first segment is the mount name. Anything else is relative to FolderMountPath (the folder of
 * the file that holds the reference, a mount root included). '\' separates like '/'; empty and "." segments
 * are dropped; ".." removes the previous segment. Returns a well-formed mount path (IsWellFormedMountPath),
 * or nullopt when FolderMountPath is not well-formed, Reference is empty, or a ".." would climb above the
 * mount root ("/Project" + "../Engine/x" is not "/Engine/x"). Reference "." names the folder itself. Pure
 * text: the caller decodes URI escapes, rejects URI schemes (data:, file:) and splits a "#label" off first.
 */
[[nodiscard]] inline std::optional<FString> ResolveMountPath (FStringView FolderMountPath, FStringView Reference) noexcept
{
	if (!IsWellFormedMountPath(FolderMountPath) || Reference.IsEmpty())
	{
		return std::nullopt;
	}
	const auto isSeparator = [] (char Ch) { return Ch == '/' || Ch == '\\'; };

	FString path;
	uint32 rootLength = 0u;      // "/<MountName>": ".." never removes it
	uint32 position = 0u;        // the next unread byte of Reference
	if (isSeparator(Reference[0]))
	{
		position = 1u;
		while (position < Reference.Len() && !isSeparator(Reference[position]))
		{
			++position;
		}
		const FStringView mountName = Reference.Mid(1u, position - 1u);
		if (mountName.IsEmpty() || mountName == "." || mountName == "..")
		{
			return std::nullopt;
		}
		path.Reserve(Reference.Len());
		path += '/';
		path += mountName;
		rootLength = path.Len();
	}
	else
	{
		path.Reserve(FolderMountPath.Len() + 1u + Reference.Len());
		path += FolderMountPath;
		rootLength = 1u + FolderMountPath.RightChop(1u).SplitFirst('/').Left.Len();
	}

	while (position < Reference.Len())
	{
		const uint32 start = position;
		while (position < Reference.Len() && !isSeparator(Reference[position]))
		{
			++position;
		}
		const FStringView segment = Reference.Mid(start, position - start);
		++position;   // past the separator (or the end)
		if (segment.IsEmpty() || segment == ".")
		{
			continue;
		}
		if (segment == "..")
		{
			if (path.Len() == rootLength)
			{
				return std::nullopt;
			}
			const uint32 parentLength = GetMountPathParent(path.View()).Len();
			path.LeftInline(parentLength);
			continue;
		}
		path += '/';
		path += segment;
	}
	return path;
}
}


FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EFileClass,
	FRT_ENUM_ENTRY(frt::assets::EFileClass, Asset),
	FRT_ENUM_ENTRY(frt::assets::EFileClass, Companion),
	FRT_ENUM_ENTRY(frt::assets::EFileClass, Raw),
	FRT_ENUM_ENTRY(frt::assets::EFileClass, Unclaimed),
	FRT_ENUM_ENTRY(frt::assets::EFileClass, Excluded),
	FRT_ENUM_ENTRY(frt::assets::EFileClass, Meta));

FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EMetaState,
	FRT_ENUM_ENTRY(frt::assets::EMetaState, Paired),
	FRT_ENUM_ENTRY(frt::assets::EMetaState, PendingMeta),
	FRT_ENUM_ENTRY(frt::assets::EMetaState, Orphan),
	FRT_ENUM_ENTRY(frt::assets::EMetaState, Broken),
	FRT_ENUM_ENTRY(frt::assets::EMetaState, MissingMeta));

// One entry per bit (no None): a combination prints bit by bit.
FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ERecordFlags,
	FRT_ENUM_ENTRY(frt::assets::ERecordFlags, Primary),
	FRT_ENUM_ENTRY(frt::assets::ERecordFlags, HasProblems),
	FRT_ENUM_ENTRY(frt::assets::ERecordFlags, StaleTags),
	FRT_ENUM_ENTRY(frt::assets::ERecordFlags, LoadedThroughRedirect),
	FRT_ENUM_ENTRY(frt::assets::ERecordFlags, Unsaved),
	FRT_ENUM_ENTRY(frt::assets::ERecordFlags, EditorOnly),
	FRT_ENUM_ENTRY(frt::assets::ERecordFlags, PinnedId),
	FRT_ENUM_ENTRY(frt::assets::ERecordFlags, Schemaless),
	FRT_ENUM_ENTRY(frt::assets::ERecordFlags, LegacyReferences),
	FRT_ENUM_ENTRY(frt::assets::ERecordFlags, Aliased),
	FRT_ENUM_ENTRY(frt::assets::ERecordFlags, TypeExcluded));

FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ETagType,
	FRT_ENUM_ENTRY(frt::assets::ETagType, None),
	FRT_ENUM_ENTRY(frt::assets::ETagType, Int64),
	FRT_ENUM_ENTRY(frt::assets::ETagType, Double),
	FRT_ENUM_ENTRY(frt::assets::ETagType, Bool),
	FRT_ENUM_ENTRY(frt::assets::ETagType, Name),
	FRT_ENUM_ENTRY(frt::assets::ETagType, String),
	FRT_ENUM_ENTRY(frt::assets::ETagType, Enum),
	FRT_ENUM_ENTRY(frt::assets::ETagType, AssetRef),
	FRT_ENUM_ENTRY(frt::assets::ETagType, RowRef));

// One entry per bit (no None).
FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ETagFlags,
	FRT_ENUM_ENTRY(frt::assets::ETagFlags, CookToRuntime),
	FRT_ENUM_ENTRY(frt::assets::ETagFlags, MultiValue),
	FRT_ENUM_ENTRY(frt::assets::ETagFlags, Hidden),
	FRT_ENUM_ENTRY(frt::assets::ETagFlags, AfterBuild),
	FRT_ENUM_ENTRY(frt::assets::ETagFlags, EditorOnly));

// One entry per bit.
FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EDepFlags,
	FRT_ENUM_ENTRY(frt::assets::EDepFlags, Hard),
	FRT_ENUM_ENTRY(frt::assets::EDepFlags, EditorOnly),
	FRT_ENUM_ENTRY(frt::assets::EDepFlags, Build),
	FRT_ENUM_ENTRY(frt::assets::EDepFlags, Row),
	FRT_ENUM_ENTRY(frt::assets::EDepFlags, Unresolved),
	FRT_ENUM_ENTRY(frt::assets::EDepFlags, Declared),
	FRT_ENUM_ENTRY(frt::assets::EDepFlags, Bundle),
	FRT_ENUM_ENTRY(frt::assets::EDepFlags, Schemaless),
	FRT_ENUM_ENTRY(frt::assets::EDepFlags, Path),
	FRT_ENUM_ENTRY(frt::assets::EDepFlags, Code));

FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ECookRule,
	FRT_ENUM_ENTRY(frt::assets::ECookRule, Auto),
	FRT_ENUM_ENTRY(frt::assets::ECookRule, Always),
	FRT_ENUM_ENTRY(frt::assets::ECookRule, Never),
	FRT_ENUM_ENTRY(frt::assets::ECookRule, EditorOnly),
	FRT_ENUM_ENTRY(frt::assets::ECookRule, DevelopmentOnly));

FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EImportStatus,
	FRT_ENUM_ENTRY(frt::assets::EImportStatus, NotAnalyzed),
	FRT_ENUM_ENTRY(frt::assets::EImportStatus, Analyzed),
	FRT_ENUM_ENTRY(frt::assets::EImportStatus, AnalyzeFailed),
	FRT_ENUM_ENTRY(frt::assets::EImportStatus, ImporterUnavailable));

FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ECompare,
	FRT_ENUM_ENTRY(frt::assets::ECompare, Eq),
	FRT_ENUM_ENTRY(frt::assets::ECompare, Ne),
	FRT_ENUM_ENTRY(frt::assets::ECompare, Lt),
	FRT_ENUM_ENTRY(frt::assets::ECompare, Le),
	FRT_ENUM_ENTRY(frt::assets::ECompare, Gt),
	FRT_ENUM_ENTRY(frt::assets::ECompare, Ge),
	FRT_ENUM_ENTRY(frt::assets::ECompare, In),
	FRT_ENUM_ENTRY(frt::assets::ECompare, Prefix),
	FRT_ENUM_ENTRY(frt::assets::ECompare, Contains),
	FRT_ENUM_ENTRY(frt::assets::ECompare, Exists),
	FRT_ENUM_ENTRY(frt::assets::ECompare, Missing),
	FRT_ENUM_ENTRY(frt::assets::ECompare, Match));

FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::EExists,
	FRT_ENUM_ENTRY(frt::assets::EExists, No),
	FRT_ENUM_ENTRY(frt::assets::EExists, Yes),
	FRT_ENUM_ENTRY(frt::assets::EExists, Unknown));

FRT_DECLARE_ENUM_REFLECTION(
	frt::assets::ERecordScope,
	FRT_ENUM_ENTRY(frt::assets::ERecordScope, Assets),
	FRT_ENUM_ENTRY(frt::assets::ERecordScope, Rows),
	FRT_ENUM_ENTRY(frt::assets::ERecordScope, SubAssets),
	FRT_ENUM_ENTRY(frt::assets::ERecordScope, All));
