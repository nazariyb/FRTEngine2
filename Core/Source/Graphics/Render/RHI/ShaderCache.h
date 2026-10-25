#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Assets/AssetId.h"
#include "Assets/BuilderGuard.h"
#include "Assets/DerivedDataCache.h"
#include "Containers/String.h"
#include "Graphics/Render/RHI/ShaderCompiler.h"
#include "Hash/StableHash.h"
#include "Platform/FileSystem.h"


/**
 * The shader cache: the derived-data cache's first client (design §14.5, §7.4), between
 * CPipelineLibrary and CShaderCompiler. A warm start reads every program's DXIL back instead of
 * compiling it with DXC at -O3, one after the other; a fresh clone, another worktree or a branch
 * switched back hits what any of them built on this machine.
 *
 * The key is the DDC's two-step key. The StaticKey hashes everything known before a compile - the
 * KeyBlob below - and the includes, known only after one, are the discovered inputs: the manifest of
 * a StaticKey keeps up to 4 sets of them, each with the Key it gave, so editing an include misses, and
 * reverting it hits again.
 *
 *   KeyBlob of Builder "Shader.Dxil" (Assets/KeyWriter.h; the golden key-schema test in
 *   Core-Test/Graphics/ShaderCacheTest.cpp pins its fields):
 *     Builder         Str     "Shader.Dxil"
 *     Version         Guid    GetShaderDxilBuilderVersion()
 *     BuildSystem     Guid    assets::BuildSystemVersion
 *     Target          Record  { Rhi: Str "D3D12" }: DXIL depends on the shader model in the profile, never
 *                             on the GPU (§7.13)
 *     Compiler        Record  { Name: Str "DXC", Dxcompiler, Dxil, VersionText: Hash160 } (CShaderCompiler::
 *                             GetDxcIdentity): the in-process compiler's identity (§7.4 "Compiler"); with
 *                             any of the three unknown there is no key, and the program compiles uncached
 *     Settings        Record  { Entry: Str (omitted for a library), Profile: Str,
 *                               Defines: Record { <name>: Str <value> } (omitted when none),
 *                               ExtraArguments: Record { "0000": Str, ... } (omitted when none),
 *                               FixedArguments: Record { "0000": Str, ... } }
 *     Source          Record  { Hash: Hash160, Size: U64 } of the main file's bytes as stored
 *     Discriminators  Record  { DebugInfoRoot: Hash160 } with debug information (see below), and
 *                             { LocalSource: Str } while the builder's sources differ from their committed
 *                             hash (Assets/BuilderGuard.h); omitted when neither applies
 *
 * FixedArguments is CShaderCompiler::MakeFixedArguments with the shader root and the source's
 * directory written as mount paths ("-I", "/Engine/Shaders/Scene"), so the key is the command line
 * Compile runs, with no absolute path in it (§7.4 "Never in a key"). Lists are records whose field
 * names are four-digit indices, so their order is kept. Defines are a record by name, and Compile
 * passes them to DXC in that same name order, so the record is the command line DXC sees (its debug
 * information embeds it): their order in the description does not matter, and a name given twice is a
 * key error, which compiles that program without the cache.
 *
 * Discovered inputs: Kind "File", Name the include's mount path ("/Engine/Shaders/Common/Math.hlsli"),
 * Hash and Size of the bytes the compile read (SShaderFileRead) - never re-read afterwards, so a file
 * saved during a compile cannot make its output pass for the new content; and Kind "Absent" (no hash,
 * size 0) for each place DXC looked for an include and found nothing (SShaderBinary::FilesMissing),
 * which must still be empty: a header created there would be included instead. A place that held
 * something DXC could not read and looked past (a directory, a file held without read sharing) cannot
 * be keyed either way, so such a compile is not stored (SShaderBinary::bInputsStable). The shader root is
 * mounted at RootMountPath; a source or include outside it has no path-free name, and such a program
 * is compiled without the cache.
 *
 * Debug information (-Zi -Qembed_debug: Debug and Development builds) embeds the absolute paths of
 * the sources in the DXIL. That variant adds BLAKE3-160 of the shader root's absolute path to its key
 * (Discriminators.DebugInfoRoot) and is stored with bDeterministic = false, so worktrees on one
 * machine never hand each other DXIL that names the other's files, and it never leaves the local
 * store. Without debug information keys hold no path at all, and every checkout shares them.
 *
 * A record holds the DXIL as the value "Dxil", and the compiler's warnings, when there were any, as
 * "Log" (printed again on a hit, as a compile would). A failed compile writes nothing (§7.9). On a
 * hit, -Ddc:Verify=<percent> (VerifyPercent) compiles that share of the programs again - chosen by
 * their Key, so the same programs on every run - and compares the bytes: a difference is BUILD002,
 * logged, and the fresh DXIL is used and stored.
 *
 * Thread-safe: programs may be compiled from several threads (CShaderCompiler serializes DXC itself);
 * the counters are atomics, the file-hash memo has its own lock, and the DDC is thread-safe. Release
 * builds open no DDC (CEngine), so their cache only compiles - until the cook ships DXIL (M12).
 */
namespace frt::graphics::rhi
{
// The builder name in every Shader.Dxil KeyBlob and record, and the DDC bucket of its records.
inline constexpr FStringLiteral ShaderDxilBuilder = "Shader.Dxil";
inline constexpr FStringLiteral ShaderCacheBucket = "Shader";
// Where the compiler's shader root is mounted, in keys: <engine>/Core/Content is /Engine.
inline constexpr FStringLiteral EngineShaderMountPath = "/Engine/Shaders";

// The Shader.Dxil builder's version (§7.3): its row in Assets/BuilderVersions.inl, through the builder
// source guard. While the row's sources (ShaderCompiler.*, ShaderCache.*, Platform/TextDecoding.cpp and
// the key infrastructure) differ from its committed hash, bLocalOnly is set: keys carry
// Discriminators.LocalSource and records stay on this machine.
[[nodiscard]] FRT_CORE_API assets::SBuilderKeyVersion GetShaderDxilKeyVersion () noexcept;
// Its Version field alone.
[[nodiscard]] FRT_CORE_API assets::SGuid GetShaderDxilBuilderVersion () noexcept;

// What the cache did since Init (or ResetCounters).
struct SShaderCacheCounters
{
	uint64 Hits = 0u;               // programs read from the DDC
	uint64 Misses = 0u;             // looked up and not found: compiled (and stored, if that worked)
	uint64 Compiled = 0u;           // programs DXC compiled: the misses, and compiles without the cache
	uint64 Uncached = 0u;           // ... of those, without the cache: none open, -Ddc:Shaders=0, no path-free key
	uint64 Failed = 0u;             // compiles that failed (and wrote nothing)
	uint64 NotStored = 0u;          // compiled after a miss, but not stored: a file changed meanwhile or could not
	                                // be read, a write failed
	uint64 VerifyCompiles = 0u;     // hits compiled again by -Ddc:Verify
	uint64 VerifyMismatches = 0u;   // ... whose DXIL differed from the cached bytes (BUILD002)
	uint64 Microseconds = 0u;       // spent in Compile: lookups, reads and compiles alike
};

struct SShaderCacheSettings
{
	// The store; null compiles every program with DXC (Release, -Ddc:Shaders=0, or no store opened).
	assets::CDerivedDataCache* Cache = nullptr;
	// -Ddc:Verify: the share of hits compiled again and compared, in percent (0 to 100).
	float VerifyPercent = 0.0f;
	// The mount path of the compiler's shader root in keys (tests mount a temporary root the same way).
	FString RootMountPath = FString(EngineShaderMountPath);
};

// A program's key: the KeyBlob, the StaticKey derived from it, and the main source it hashed.
struct SShaderCacheKey
{
	std::vector<uint8> KeyBlob;
	hash::SContentHash StaticKey;
	SShaderFileRead Source;
	// The builder version the KeyBlob was written with: what a record stored under it says, local-only
	// included, whatever the builder source guard's switch says by then.
	assets::SBuilderKeyVersion Version;
};


class FRT_CORE_API CShaderCache
{
public:
	void Init (CShaderCompiler& Compiler, const SShaderCacheSettings& Settings);

	// The program's DXIL: from the DDC when it holds it for the current sources, else compiled (and
	// stored). A hit comes back with its watched files in Dependencies, the source and every include
	// the matched input set names, exactly as a compile reports them.
	SShaderBinary Compile (const SShaderCompileDesc& Desc);

	// The key Compile looks Desc up with: nullopt, with the reason, when it has none (the source cannot
	// be read or lies outside the shader root, a define is named twice). For tests and diagnostics.
	[[nodiscard]] std::optional<SShaderCacheKey> MakeKey (const SShaderCompileDesc& Desc,
		FString* OutWhyNot = nullptr) const;

	[[nodiscard]] bool IsCaching () const noexcept { return Settings.Cache != nullptr; }
	[[nodiscard]] SShaderCacheCounters GetCounters () const noexcept;
	void ResetCounters () noexcept;
	// "<hits> hits, <compiled> compiled (<ms> ms)", the start-up line's text (and the Stats panel's).
	[[nodiscard]] static FString FormatCounters (const SShaderCacheCounters& Counters);

	CShaderCompiler& GetCompiler () { return *Compiler; }

private:
	struct SAtomicCounters
	{
		std::atomic<uint64> Hits = 0u;
		std::atomic<uint64> Misses = 0u;
		std::atomic<uint64> Compiled = 0u;
		std::atomic<uint64> Uncached = 0u;
		std::atomic<uint64> Failed = 0u;
		std::atomic<uint64> NotStored = 0u;
		std::atomic<uint64> VerifyCompiles = 0u;
		std::atomic<uint64> VerifyMismatches = 0u;
		std::atomic<uint64> Microseconds = 0u;
	};

	// The last hash of a file a lookup read, valid while Stat still reports the same size, write time
	// and identity: a warm start reads each include once, not once per program that includes it. The
	// trust the registry's scan gives the same three facts (§5.4).
	struct SFileHashMemo
	{
		platform::SFileStat Stat;
		hash::SContentHash Hash;
	};

	// The cache's work for Desc; Compile adds the time it took.
	SShaderBinary Lookup (const SShaderCompileDesc& Desc);
	// A compile without the cache.
	SShaderBinary CompileUncached (const SShaderCompileDesc& Desc);
	// A compile after a miss, stored under Key when its inputs allow.
	SShaderBinary CompileAndStore (const SShaderCompileDesc& Desc, const SShaderCacheKey& Key);
	// Stores a successful compile of Key's program: false (and counted) when it read a source other
	// than the one Key hashed, a file changed during it, a place it looked at for an include could not
	// be read, an include lies outside the shader root, or the write failed.
	bool Store (const SShaderCacheKey& Key, const SShaderBinary& Binary, uint64 BuildMs, FStringView Label);
	// A hit -Ddc:Verify chose: compiled again and compared; the fresh binary when the bytes differ.
	SShaderBinary VerifyHit (const SShaderCompileDesc& Desc, const SShaderCacheKey& Key, SShaderBinary Hit,
		const assets::SDdcManifestMatch& Match, FStringView Label);
	// The record's DXIL as a binary, with the watched files of the matched inputs. nullopt when a
	// recorded input has no path or DXC cannot hold the bytes.
	std::optional<SShaderBinary> MakeHit (const SShaderCacheKey& Key, const std::vector<uint8>& Dxil, FStringView Log,
		const std::vector<assets::SDdcInput>& Inputs) const;
	// Whether -Ddc:Verify compiles the program of this Key again.
	bool IsVerified (const hash::SContentHash& Key) const noexcept;
	// The discovered inputs of a compile - every include, by mount path - or nullopt when one has none.
	std::optional<std::vector<assets::SDdcInput>> MakeInputs (const SShaderBinary& Binary) const;

	// "/Engine/Shaders/Common/Math.hlsli" for a file under the shader root; nullopt outside it.
	std::optional<FString> ToMountPath (const std::filesystem::path& Path) const;
	// The file a mount path names; nullopt for one outside RootMountPath or with a "." or ".." in it.
	std::optional<std::filesystem::path> FromMountPath (FStringView MountPath) const;
	// Whether a recorded input still has its recorded content.
	bool IsInputCurrent (const assets::SDdcInput& Recorded) const;
	// The current hash and size of a file (through the memo); nullopt when it cannot be read whole.
	std::optional<SShaderFileRead> HashCurrentFile (const std::filesystem::path& Path) const;

	CShaderCompiler* Compiler = nullptr;
	SShaderCacheSettings Settings;
	mutable SAtomicCounters Counters;

	mutable std::mutex MemoMutex;
	mutable std::map<std::filesystem::path, SFileHashMemo> Memo;
};
}
