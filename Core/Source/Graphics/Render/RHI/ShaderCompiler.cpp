#include "Graphics/Render/RHI/ShaderCompiler.h"

#include <Windows.h>

#include <cstdio>
#include <optional>
#include <set>

#include "BuildConfig.h"
#include "Containers/WideString.h"
#include "Platform/Platform.h"


namespace frt::graphics::rhi
{
namespace
{
// Records every file the compiler opens. Resolution order: the path as DXC computed it (it
// already joins the including file's directory), then the shader root.
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

		std::filesystem::path requested = Filename;
		std::error_code ec;
		std::filesystem::path resolved;
		if (std::filesystem::exists(requested, ec))
		{
			resolved = requested;
		}
		else
		{
			const std::filesystem::path underRoot = Root / requested.relative_path();
			if (std::filesystem::exists(underRoot, ec))
			{
				resolved = underRoot;
			}
			else
			{
				const std::filesystem::path byName = Root / requested.filename();
				if (std::filesystem::exists(byName, ec))
				{
					resolved = byName;
				}
			}
		}

		if (resolved.empty())
		{
			return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
		}

		resolved = std::filesystem::weakly_canonical(resolved, ec);
		ComPtr<IDxcBlobEncoding> blob;
		UINT32 codePage = DXC_CP_UTF8;
		const HRESULT hr = Utils->LoadFile(resolved.c_str(), &codePage, &blob);
		if (FAILED(hr))
		{
			return hr;
		}
		Opened.insert(resolved);
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

	std::set<std::filesystem::path> Opened;

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
}


bool CShaderCompiler::Init (const std::filesystem::path& InShaderRoot)
{
	std::error_code ec;
	ShaderRoot = std::filesystem::weakly_canonical(InShaderRoot, ec);
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
	UINT32 codePage = DXC_CP_UTF8;
	if (FAILED(Utils->LoadFile(sourcePath.c_str(), &codePage, &source)))
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
	std::vector<FWideString> args;
	args.reserve(20u + 2u * Desc.Defines.size() + Desc.ExtraArguments.size());
	args.emplace_back(sourcePath.filename().native());
	if (!Desc.EntryPoint.IsEmpty())
	{
		args.emplace_back("-E");
		args.emplace_back(Desc.EntryPoint);
	}
	args.emplace_back("-T");
	args.emplace_back(Desc.Profile);
	args.emplace_back("-HV");
	args.emplace_back("2021");
	args.emplace_back("-Zpc");
	args.emplace_back("-O3");
	if (ProfileSupports16Bit(Desc.Profile))
	{
		args.emplace_back("-enable-16bit-types");
	}
	args.emplace_back("-I");
	args.emplace_back(ShaderRoot.native());
	args.emplace_back("-I");
	args.emplace_back(sourcePath.parent_path().native());
#if FRT_SHADER_DEBUG_INFO
	args.emplace_back("-Zi");
	args.emplace_back("-Qembed_debug");
#else
	args.emplace_back("-Qstrip_debug");
	args.emplace_back("-Qstrip_reflect");
#endif
	for (const SShaderDefine& define : Desc.Defines)
	{
		args.emplace_back("-D");
		args.emplace_back(FString::Format("{}={}", define.Name, define.Value));
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
	for (const std::filesystem::path& dep : includeHandler.Opened)
	{
		if (dep != sourcePath)
		{
			result.Dependencies.push_back(dep);
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
