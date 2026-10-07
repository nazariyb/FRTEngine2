#pragma once

#include "Graphics/Render/RHI/RHICommon.h"

#include <Windows.h>
#include <dxcapi.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "Graphics/Render/RHI/RHICommon.h"


namespace frt::graphics::rhi
{
struct SShaderDefine
{
	std::string Name;
	std::string Value = "1";
};

struct SShaderCompileDesc
{
	std::filesystem::path SourcePath;
	std::string EntryPoint;            // empty for libraries
	std::string Profile;               // e.g. cs_6_6, lib_6_9, vs_6_6
	std::vector<SShaderDefine> Defines;
	std::vector<std::wstring> ExtraArguments;
};

struct SShaderBinary
{
	ComPtr<IDxcBlob> Blob;
	std::vector<std::filesystem::path> Dependencies; // source + every include actually opened
	std::string Log;
	bool bSuccess = false;

	D3D12_SHADER_BYTECODE GetBytecode () const
	{
		return Blob ? D3D12_SHADER_BYTECODE{ Blob->GetBufferPointer(), Blob->GetBufferSize() }
					: D3D12_SHADER_BYTECODE{ nullptr, 0 };
	}
};

/**
 * DXC front end. IDxcCompiler3 with an include handler that records every file it opens, so
 * hot reload can watch headers and not only the top-level source.
 *
 * Fixed arguments: -HV 2021, -Zpc (column-major matrices; CPU uploads XMFLOAT4X4 as stored and
 * shaders use mul(M, v)), -enable-16bit-types where the profile allows it, -O3, all warnings.
 * Debug builds add -Zi -Qembed_debug; release strips debug and reflection.
 */
class FRT_CORE_API CShaderCompiler
{
public:
	bool Init (const std::filesystem::path& ShaderRoot);
	SShaderBinary Compile (const SShaderCompileDesc& Desc);

	const std::filesystem::path& GetShaderRoot () const { return ShaderRoot; }

	// The newest compile error, kept for the UI. Cleared by a successful compile of the same file.
	std::string GetLastErrorLog () const;

private:
	ComPtr<IDxcUtils> Utils;
	ComPtr<IDxcCompiler3> Compiler;
	std::filesystem::path ShaderRoot;
	mutable std::mutex Mutex;
	std::string LastErrorLog;
};
}
