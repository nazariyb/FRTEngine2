#include "Graphics/Render/RHI/ShaderCompiler.h"

#include <Windows.h>

#include <cstdio>
#include <set>

#include "BuildConfig.h"


namespace frt::graphics::rhi
{
namespace
{
std::string Narrow (const std::wstring& Text)
{
	std::string out;
	out.reserve(Text.size());
	for (const wchar_t c : Text)
	{
		out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
	}
	return out;
}

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
		const HRESULT hr = Utils->LoadFile(resolved.wstring().c_str(), nullptr, &blob);
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

bool ProfileSupports16Bit (const std::string& Profile)
{
	// _6_2 and newer accept -enable-16bit-types.
	const size_t underscore = Profile.find('_');
	if (underscore == std::string::npos || Profile.size() < underscore + 4)
	{
		return false;
	}
	const int major = Profile[underscore + 1] - '0';
	const int minor = std::atoi(Profile.c_str() + underscore + 3);
	return major > 6 || (major == 6 && minor >= 2);
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

std::string CShaderCompiler::GetLastErrorLog () const
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
	if (FAILED(Utils->LoadFile(sourcePath.wstring().c_str(), nullptr, &source)))
	{
		result.Log = "Cannot open shader source: " + sourcePath.string();
		result.Dependencies.push_back(sourcePath);
		LastErrorLog = result.Log;
		std::fprintf(stderr, "[shader] %s\n", result.Log.c_str());
		return result;
	}

	std::vector<std::wstring> args;
	args.push_back(sourcePath.filename().wstring());
	if (!Desc.EntryPoint.empty())
	{
		args.push_back(L"-E");
		args.push_back(Widen(Desc.EntryPoint));
	}
	args.push_back(L"-T");
	args.push_back(Widen(Desc.Profile));
	args.push_back(L"-HV");
	args.push_back(L"2021");
	args.push_back(L"-Zpc");
	args.push_back(L"-O3");
	if (ProfileSupports16Bit(Desc.Profile))
	{
		args.push_back(L"-enable-16bit-types");
	}
	args.push_back(L"-I");
	args.push_back(ShaderRoot.wstring());
	args.push_back(L"-I");
	args.push_back(sourcePath.parent_path().wstring());
#if FRT_SHADER_DEBUG_INFO
	args.push_back(L"-Zi");
	args.push_back(L"-Qembed_debug");
#else
	args.push_back(L"-Qstrip_debug");
	args.push_back(L"-Qstrip_reflect");
#endif
	for (const SShaderDefine& define : Desc.Defines)
	{
		args.push_back(L"-D");
		args.push_back(Widen(define.Name + "=" + define.Value));
	}
	for (const std::wstring& extra : Desc.ExtraArguments)
	{
		args.push_back(extra);
	}

	std::vector<LPCWSTR> argPtrs;
	argPtrs.reserve(args.size());
	for (const std::wstring& arg : args)
	{
		argPtrs.push_back(arg.c_str());
	}

	DxcBuffer sourceBuffer = {};
	sourceBuffer.Ptr = source->GetBufferPointer();
	sourceBuffer.Size = source->GetBufferSize();
	sourceBuffer.Encoding = DXC_CP_ACP;

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
			result.Log = errors->GetStringPointer();
		}
	}

	HRESULT status = E_FAIL;
	if (SUCCEEDED(hr) && compileResult)
	{
		compileResult->GetStatus(&status);
	}

	if (FAILED(status))
	{
		const std::string header = "[shader] FAILED " + sourcePath.filename().string() +
			(Desc.EntryPoint.empty() ? "" : (":" + Desc.EntryPoint)) + " (" + Desc.Profile + ")\n";
		LastErrorLog = header + result.Log;
		std::fprintf(stderr, "%s%s\n", header.c_str(), result.Log.c_str());
		std::fflush(stderr);
		OutputDebugStringA((header + result.Log).c_str());
		return result;
	}

	if (!result.Log.empty())
	{
		// Warnings: surface them but do not fail.
		std::fprintf(stdout, "[shader] warnings in %s:\n%s\n", sourcePath.filename().string().c_str(), result.Log.c_str());
	}

	compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&result.Blob), nullptr);
	result.bSuccess = result.Blob && result.Blob->GetBufferSize() > 0;
	return result;
}
}
