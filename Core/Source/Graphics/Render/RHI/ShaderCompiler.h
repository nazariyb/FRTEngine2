#pragma once

#include "Graphics/Render/RHI/RHICommon.h"

#include <Windows.h>
#include <dxcapi.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "BuildConfig.h"
#include "Containers/String.h"
#include "Hash/StableHash.h"


namespace frt::graphics::rhi
{
struct SShaderDefine
{
	FString Name;
	FString Value = "1";
};

struct SShaderCompileDesc
{
	std::filesystem::path SourcePath;
	FString EntryPoint;                // empty for libraries
	FString Profile;                   // e.g. cs_6_6, lib_6_9, vs_6_6
	std::vector<SShaderDefine> Defines;    // passed to DXC in name order, whatever their order here
	std::vector<FString> ExtraArguments;   // passed to DXC after the fixed arguments
};

// A file a compile read, and what it read: the BLAKE3-160 and size of the file's bytes as stored -
// the very bytes DXC was given (after their encoding was detected), so a cache keyed on these hashes
// can never describe content other than what was compiled, however the file changes meanwhile.
struct SShaderFileRead
{
	std::filesystem::path Path;   // absolute, canonical
	hash::SContentHash Hash;
	uint64 Size = 0u;
};

struct SShaderBinary
{
	ComPtr<IDxcBlob> Blob;
	std::vector<std::filesystem::path> Dependencies; // source + every include actually opened
	// The source first, then every include it opened, each once (in path order). A compile served from
	// the shader cache leaves it empty, as it leaves FilesMissing.
	std::vector<SShaderFileRead> FilesRead;
	// Every place DXC looked for an include and found nothing (absolute, canonical, in path order): a
	// file created at one of them would be included instead, and change the output.
	std::vector<std::filesystem::path> FilesMissing;
	FString Log;
	bool bSuccess = false;
	// False when the sources changed during the compile - one file read twice gave different bytes, or
	// a place found empty was then found to hold a file - or when a place DXC looked at for an include
	// held something it could not read (a directory, a file held without read sharing) and DXC looked on
	// past it: the output then belongs to no single version of the sources, and must not be cached.
	bool bInputsStable = true;

	D3D12_SHADER_BYTECODE GetBytecode () const
	{
		return Blob ? D3D12_SHADER_BYTECODE{ Blob->GetBufferPointer(), Blob->GetBufferSize() }
					: D3D12_SHADER_BYTECODE{ nullptr, 0 };
	}
};

// What identifies the DXC a process compiles with (design §14.5): the BLAKE3-160 of dxcompiler.dll and
// dxil.dll as loaded (dxil.dll validates and signs the DXIL, so it changes the output too) and of
// ThirdParty/Dxc/VERSION.txt, which says which release was vendored. A hash is unset (IsValid false)
// when its file was not found.
struct SDxcIdentity
{
	hash::SContentHash Dxcompiler;
	hash::SContentHash Dxil;
	hash::SContentHash VersionText;
};

/**
 * DXC front end. IDxcCompiler3 with an include handler that records every file it opens, so
 * hot reload can watch headers and not only the top-level source, and the shader cache can key on
 * them (ShaderCache.h).
 *
 * Fixed arguments: -HV 2021, -Zpc (column-major matrices; CPU uploads XMFLOAT4X4 as stored and
 * shaders use mul(M, v)), -enable-16bit-types where the profile allows it, -O3, all warnings.
 * With debug information (Debug and Development builds by default: FRT_SHADER_DEBUG_INFO) -Zi
 * -Qembed_debug; without, debug data and reflection are stripped. The defines follow in name order,
 * whatever order the description lists them in: the debug information embeds the command line, and
 * one set of defines must give one program.
 */
class FRT_CORE_API CShaderCompiler
{
public:
	// The build's default: -Zi -Qembed_debug in Debug and Development, stripped in Release.
	static constexpr bool DefaultDebugInfo = FRT_SHADER_DEBUG_INFO != 0;

	bool Init (const std::filesystem::path& ShaderRoot, bool bDebugInfo = DefaultDebugInfo);
	SShaderBinary Compile (const SShaderCompileDesc& Desc);

	const std::filesystem::path& GetShaderRoot () const { return ShaderRoot; }
	// Whether outputs carry debug information, which embeds the absolute paths of the sources.
	bool HasDebugInfo () const { return bDebugInfo; }

	/**
	 * Every argument Compile passes to DXC other than the entry point (-E), the profile (-T), the
	 * defines (-D) and Desc.ExtraArguments, in the order Compile passes them: the source's file name
	 * first, then the fixed flags, the include directories (RootDir, then SourceDir) and the debug
	 * flags. Compile calls it with the absolute shader root and source directory; the shader cache calls
	 * it with their mount paths ("/Engine/Shaders", "/Engine/Shaders/Scene"), so its keys hold exactly
	 * the command line with no absolute path in it - and cannot drift from what Compile runs.
	 */
	std::vector<FString> MakeFixedArguments (FStringView SourceName, FStringView Profile, FStringView RootDir,
		FStringView SourceDir) const;

	// A DXC blob holding a copy of Bytes: a shader the cache read back, handed out like a compiled one.
	ComPtr<IDxcBlob> CreateBlob (std::span<const uint8> Bytes) const;

	// The DXC this process loaded; hashed on the first call, then the same for the life of the process.
	static const SDxcIdentity& GetDxcIdentity ();

	// The newest compile error, kept for the UI. Cleared by a successful compile of the same file.
	// A copy: the log is written under Mutex by whichever thread compiles.
	FString GetLastErrorLog () const;

private:
	ComPtr<IDxcUtils> Utils;
	ComPtr<IDxcCompiler3> Compiler;
	std::filesystem::path ShaderRoot;
	bool bDebugInfo = DefaultDebugInfo;
	mutable std::mutex Mutex;
	FString LastErrorLog;
};
}
