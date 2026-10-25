#include "Graphics/Render/RHI/ShaderCompiler.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <set>

#include "EnginePaths.h"
#include "Containers/WideString.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"


namespace frt::graphics::rhi
{
namespace
{
// A shader source or include file as a UTF-8 blob DXC owns (CreateBlob copies), and what was read: the
// file's bytes as stored are hashed, then decoded like every other text file the engine reads - a
// UTF-8 BOM is stripped and UTF-16 is transcoded, so a file an editor saved "with signature" or as
// "Unicode" compiles - from the same read, so the hash describes exactly the text DXC compiles. Not
// IDxcUtils::LoadFile: given a code page, DXC 1.9 drops a BOM by offsetting the blob's pointer and
// later frees the offset pointer (heap corruption), and given none it leaves BOM-less text to the ANSI
// code page.
HRESULT LoadSourceBlob (IDxcUtils* Utils, const std::filesystem::path& Path, IDxcBlobEncoding** OutBlob,
	SShaderFileRead& OutRead)
{
	std::optional<FString> bytes = platform::ReadFileText(Path, ETextFileEncoding::Raw);
	if (!bytes)
	{
		return HRESULT_FROM_WIN32(ERROR_OPEN_FAILED);
	}
	OutRead.Path = Path;
	OutRead.Hash = hash::HashContent(bytes->AsBytes());
	OutRead.Size = bytes->Len();
	const std::optional<FString> text = platform::DecodeTextFile(std::move(*bytes));
	if (!text)
	{
		return HRESULT_FROM_WIN32(ERROR_OPEN_FAILED);
	}
	return Utils->CreateBlob(text->GetData(), text->Len(), DXC_CP_UTF8, OutBlob);
}

/**
 * Records every file the compiler opens, with the hash of what it read, and every file it looked for
 * and did not find. DXC asks for each place an include may be, in its search order (the including
 * file's directory, then each -I directory); the first that loads wins. Resolution of one request:
 * the path as DXC computed it when that is absolute (an -I directory joined with the name), else the
 * name under the shader root (DXC joins the including file's directory, which is "." for the main
 * file, passed by name: the root), then the bare file name under the root. Never the process's
 * working directory: what a program includes must not depend on where the process runs.
 *
 * The places that were missing matter as much as the file found: a header created at one of them
 * later would be included instead, so the shader cache keys on their absence too. A place that holds
 * something unreadable is passed over by DXC as if it were empty, but it is not absent - it becomes
 * the include as soon as it can be read - so it marks the compile unstable instead (bUnreadable).
 */
class CRecordingIncludeHandler final : public IDxcIncludeHandler
{
public:
	CRecordingIncludeHandler (IDxcUtils* InUtils, std::filesystem::path InRoot)
		: Utils(InUtils), Root(std::move(InRoot)) {}

	HRESULT STDMETHODCALLTYPE LoadSource (LPCWSTR Filename, IDxcBlob** OutSource) override
	{
		if (!OutSource)
		{
			return E_INVALIDARG;
		}
		*OutSource = nullptr;

		const std::filesystem::path requested = Filename;
		std::error_code ec;
		std::filesystem::path candidates[2];
		candidates[0] = requested.is_absolute() ? requested : Root / requested.relative_path();
		candidates[1] = Root / requested.filename();
		std::filesystem::path resolved;
		for (const std::filesystem::path& candidate : candidates)
		{
			if (std::filesystem::exists(candidate, ec))
			{
				resolved = candidate;
				break;
			}
			Missing.insert(std::filesystem::weakly_canonical(candidate, ec));
		}

		if (resolved.empty())
		{
			return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
		}

		resolved = std::filesystem::weakly_canonical(resolved, ec);
		ComPtr<IDxcBlobEncoding> blob;
		SShaderFileRead read;
		const HRESULT hr = LoadSourceBlob(Utils, resolved, &blob, read);
		if (FAILED(hr))
		{
			// Something is here that cannot be read: a directory, a file held without read sharing (an
			// editor in the middle of a save), text that does not decode. DXC looks on down the include
			// path and may find a header of that name further along, which this place shadows once it
			// can be read: the output depends on that moment, and is not cached.
			bUnreadable = true;
			return hr;
		}
		// A file opened again (include guards rather than #pragma once) must have kept its bytes.
		const auto [it, bInserted] = Opened.try_emplace(resolved, read);
		if (!bInserted && (it->second.Hash != read.Hash || it->second.Size != read.Size))
		{
			bChangedWhileRead = true;
		}
		*OutSource = blob.Detach();
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface (REFIID Riid, void** Object) override
	{
		if (Riid == __uuidof(IDxcIncludeHandler) || Riid == __uuidof(IUnknown))
		{
			*Object = static_cast<IDxcIncludeHandler*>(this);
			AddRef();
			return S_OK;
		}
		*Object = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef () override { return ++RefCount; }
	ULONG STDMETHODCALLTYPE Release () override { return --RefCount; }

	// Every file opened, in path order, with what its first read held.
	std::map<std::filesystem::path, SShaderFileRead> Opened;
	// Every place looked at and found empty (canonical).
	std::set<std::filesystem::path> Missing;
	bool bChangedWhileRead = false;
	// A place held something, and it could not be read.
	bool bUnreadable = false;

private:
	IDxcUtils* Utils = nullptr;
	std::filesystem::path Root;
	ULONG RefCount = 1;
};

bool ProfileSupports16Bit (FStringView Profile)
{
	// <stage>_<major>_<minor>: _6_2 and newer accept -enable-16bit-types.
	const SStringSplit stage = Profile.SplitFirst('_');
	const SStringSplit version = stage.Right.SplitFirst('_');
	const std::optional<int32> major = version.Left.ToInt32();
	const std::optional<int32> minor = version.Right.ToInt32();
	if (!version || !major || !minor)
	{
		return false;
	}
	return *major > 6 || (*major == 6 && *minor >= 2);
}

// BLAKE3-160 of a file's bytes; unset when it cannot be read whole and unchanged.
hash::SContentHash HashFile (const std::filesystem::path& Path)
{
	hash::CContentHasher hasher;
	const platform::EStableRead read = platform::ReadFileChunksStable(Path, [&hasher] (std::span<const uint8> Bytes)
	{
		hasher.Update(Bytes);
	});
	return read == platform::EStableRead::Stable ? hasher.Finalize() : hash::SContentHash{};
}
}


bool CShaderCompiler::Init (const std::filesystem::path& InShaderRoot, bool bInDebugInfo)
{
	std::error_code ec;
	ShaderRoot = std::filesystem::weakly_canonical(InShaderRoot, ec);
	bDebugInfo = bInDebugInfo;
	if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&Utils))))
	{
		return false;
	}
	if (FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&Compiler))))
	{
		return false;
	}
	return true;
}

FString CShaderCompiler::GetLastErrorLog () const
{
	std::lock_guard lock(Mutex);
	return LastErrorLog;
}

std::vector<FString> CShaderCompiler::MakeFixedArguments (FStringView SourceName, FStringView Profile,
	FStringView RootDir, FStringView SourceDir) const
{
	std::vector<FString> args;
	args.reserve(14u);
	args.emplace_back(SourceName);
	args.emplace_back("-HV");
	args.emplace_back("2021");
	args.emplace_back("-Zpc");
	args.emplace_back("-O3");
	if (ProfileSupports16Bit(Profile))
	{
		args.emplace_back("-enable-16bit-types");
	}
	args.emplace_back("-I");
	args.emplace_back(RootDir);
	args.emplace_back("-I");
	args.emplace_back(SourceDir);
	if (bDebugInfo)
	{
		args.emplace_back("-Zi");
		args.emplace_back("-Qembed_debug");
	}
	else
	{
		args.emplace_back("-Qstrip_debug");
		args.emplace_back("-Qstrip_reflect");
	}
	return args;
}

ComPtr<IDxcBlob> CShaderCompiler::CreateBlob (std::span<const uint8> Bytes) const
{
	std::lock_guard lock(Mutex);
	ComPtr<IDxcBlobEncoding> blob;
	if (!Utils || FAILED(Utils->CreateBlob(Bytes.data(), static_cast<UINT32>(Bytes.size()), DXC_CP_ACP, &blob)))
	{
		return nullptr;
	}
	return blob;
}

const SDxcIdentity& CShaderCompiler::GetDxcIdentity ()
{
	static const SDxcIdentity identity = []
	{
		SDxcIdentity result;
		const std::filesystem::path dxcompiler = platform::GetLoadedModulePath("dxcompiler.dll");
		if (!dxcompiler.empty())
		{
			result.Dxcompiler = HashFile(dxcompiler);
		}
		// dxcompiler.dll loads dxil.dll on its first validation, from the same search path: beside the
		// executable, where the build copies both.
		std::filesystem::path dxil = platform::GetLoadedModulePath("dxil.dll");
		if (dxil.empty() && !dxcompiler.empty())
		{
			dxil = dxcompiler.parent_path() / FStringView("dxil.dll").ToPath();
		}
		if (!dxil.empty())
		{
			result.Dxil = HashFile(dxil);
		}
		result.VersionText = HashFile(paths::GetEngineDir() / FStringView("ThirdParty/Dxc/VERSION.txt").ToPath());
		return result;
	}();
	return identity;
}

SShaderBinary CShaderCompiler::Compile (const SShaderCompileDesc& Desc)
{
	SShaderBinary result;
	std::lock_guard lock(Mutex);

	if (!Compiler || !Utils)
	{
		result.Log = "Shader compiler not initialised";
		return result;
	}

	std::error_code ec;
	const std::filesystem::path sourcePath = std::filesystem::weakly_canonical(Desc.SourcePath, ec);

	ComPtr<IDxcBlobEncoding> source;
	SShaderFileRead sourceRead;
	if (FAILED(LoadSourceBlob(Utils.Get(), sourcePath, &source, sourceRead)))
	{
		result.Log = "Cannot open shader source: " + FString::FromPath(sourcePath);
		result.Dependencies.push_back(sourcePath);
		LastErrorLog = result.Log;
		std::fprintf(stderr, "[shader] %s\n", result.Log.CStr());
		return result;
	}

	// DXC takes UTF-16 arguments: flags are UTF-8 literals like every other engine text, widened here
	// (FWideString(FStringView)). The pointers are taken once every argument is in place: a short
	// FWideString keeps its text inline, so the vector growing would move it.
	//
	//   <source name> [-E <entry>] -T <profile> <the other fixed arguments> (-D <name>=<value>)* <extra>*
	//
	// The defines go in name order (bytes, as the shader cache's key records them), whatever order Desc
	// lists them in: with debug information DXC embeds its command line in the DXIL, so two orders of one
	// set would give two programs under the one key the set has. A stable sort: a name given twice (which
	// has no key, so is never cached) keeps its last value, as DXC applies its -D in order.
	const std::vector<FString> fixed = MakeFixedArguments(FString::FromPath(sourcePath.filename()), Desc.Profile,
		FString::FromPath(ShaderRoot), FString::FromPath(sourcePath.parent_path()));
	std::vector<const SShaderDefine*> defines;
	defines.reserve(Desc.Defines.size());
	for (const SShaderDefine& define : Desc.Defines)
	{
		defines.push_back(&define);
	}
	std::stable_sort(defines.begin(), defines.end(), [] (const SShaderDefine* A, const SShaderDefine* B)
	{
		return A->Name.Compare(B->Name) < 0;
	});
	std::vector<FWideString> args;
	args.reserve(fixed.size() + 4u + 2u * Desc.Defines.size() + Desc.ExtraArguments.size());
	args.emplace_back(fixed.front());
	if (!Desc.EntryPoint.IsEmpty())
	{
		args.emplace_back("-E");
		args.emplace_back(Desc.EntryPoint);
	}
	args.emplace_back("-T");
	args.emplace_back(Desc.Profile);
	for (size_t i = 1u; i < fixed.size(); ++i)
	{
		args.emplace_back(fixed[i]);
	}
	for (const SShaderDefine* define : defines)
	{
		args.emplace_back("-D");
		args.emplace_back(FString::Format("{}={}", define->Name, define->Value));
	}
	for (const FString& extra : Desc.ExtraArguments)
	{
		args.emplace_back(extra);
	}

	std::vector<LPCWSTR> argPtrs;
	argPtrs.reserve(args.size());
	for (const FWideString& arg : args)
	{
		argPtrs.push_back(arg.CStr());
	}

	DxcBuffer sourceBuffer = {};
	sourceBuffer.Ptr = source->GetBufferPointer();
	sourceBuffer.Size = source->GetBufferSize();
	sourceBuffer.Encoding = DXC_CP_UTF8;

	CRecordingIncludeHandler includeHandler(Utils.Get(), ShaderRoot);

	ComPtr<IDxcResult> compileResult;
	const HRESULT hr = Compiler->Compile(
		&sourceBuffer, argPtrs.data(), static_cast<UINT32>(argPtrs.size()),
		&includeHandler, IID_PPV_ARGS(&compileResult));

	result.Dependencies.push_back(sourcePath);
	result.FilesRead.push_back(sourceRead);
	result.bInputsStable = !includeHandler.bChangedWhileRead && !includeHandler.bUnreadable;
	for (const auto& [dep, read] : includeHandler.Opened)
	{
		if (dep != sourcePath)
		{
			result.Dependencies.push_back(dep);
			result.FilesRead.push_back(read);
		}
		else if (read.Hash != sourceRead.Hash || read.Size != sourceRead.Size)
		{
			result.bInputsStable = false;   // the source includes itself, and changed in between
		}
	}
	for (const std::filesystem::path& missing : includeHandler.Missing)
	{
		// A place missing at one look and found at another was created during the compile.
		if (includeHandler.Opened.contains(missing) || missing == sourcePath)
		{
			result.bInputsStable = false;
		}
		else
		{
			result.FilesMissing.push_back(missing);
		}
	}

	if (compileResult)
	{
		ComPtr<IDxcBlobUtf8> errors;
		if (SUCCEEDED(compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors &&
			errors->GetStringLength() > 0)
		{
			result.Log = FStringView(errors->GetStringPointer(), errors->GetStringLength());
		}
	}

	HRESULT status = E_FAIL;
	if (SUCCEEDED(hr) && compileResult)
	{
		compileResult->GetStatus(&status);
	}

	if (FAILED(status))
	{
		FString header = "[shader] FAILED " + FString::FromPath(sourcePath.filename());
		if (!Desc.EntryPoint.IsEmpty())
		{
			header << ':' << Desc.EntryPoint;
		}
		header << " (" << Desc.Profile << ")\n";
		LastErrorLog = header + result.Log;
		std::fprintf(stderr, "%s%s\n", header.CStr(), result.Log.CStr());
		std::fflush(stderr);
		platform::OutputDebugText(LastErrorLog);
		return result;
	}

	if (!result.Log.IsEmpty())
	{
		// Warnings: surface them but do not fail.
		std::fprintf(stdout, "[shader] warnings in %s:\n%s\n", FString::FromPath(sourcePath.filename()).CStr(), result.Log.CStr());
	}

	compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&result.Blob), nullptr);
	result.bSuccess = result.Blob && result.Blob->GetBufferSize() > 0;
	return result;
}
}
