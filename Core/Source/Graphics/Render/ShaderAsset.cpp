#include "ShaderAsset.h"

#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WINDOWS
#include <Windows.h>
#endif

#include "Asserts.h"


namespace frt::graphics
{
#ifdef _WINDOWS
struct SDxcContext
{
	Microsoft::WRL::ComPtr<IDxcCompiler> Compiler;
	Microsoft::WRL::ComPtr<IDxcLibrary> Library;
	Microsoft::WRL::ComPtr<IDxcIncludeHandler> IncludeHandler;
	bool bInitialized = false;
};

static std::wstring ToWide (std::string_view Value)
{
	return std::wstring(Value.begin(), Value.end());
}

static bool IsLibraryTarget (std::string_view TargetProfile)
{
	return TargetProfile.rfind("lib_", 0) == 0;
}

static SDxcContext& GetDxcContext ()
{
	static SDxcContext context = {};
	if (context.bInitialized)
	{
		return context;
	}

	context.bInitialized = true;
	const HRESULT compilerHr = DxcCreateInstance(
		CLSID_DxcCompiler,
		__uuidof(IDxcCompiler),
		reinterpret_cast<void**>(context.Compiler.GetAddressOf()));
	const HRESULT libraryHr = DxcCreateInstance(
		CLSID_DxcLibrary,
		__uuidof(IDxcLibrary),
		reinterpret_cast<void**>(context.Library.GetAddressOf()));
	if (FAILED(compilerHr) || FAILED(libraryHr) || !context.Compiler || !context.Library)
	{
		context.Compiler.Reset();
		context.Library.Reset();
		context.IncludeHandler.Reset();
		return context;
	}

	const HRESULT includeHr = context.Library->CreateIncludeHandler(context.IncludeHandler.GetAddressOf());
	if (FAILED(includeHr) || !context.IncludeHandler)
	{
		context.Compiler.Reset();
		context.Library.Reset();
		context.IncludeHandler.Reset();
	}

	return context;
}
#endif

size_t CShaderLibrary::SStringHash::operator() (std::string_view Value) const noexcept
{
	return std::hash<std::string_view>{}(Value);
}

size_t CShaderLibrary::SStringHash::operator() (const std::string& Value) const noexcept
{
	return std::hash<std::string_view>{}(Value);
}

bool CShaderLibrary::SStringEqual::operator() (std::string_view Lhs, std::string_view Rhs) const noexcept
{
	return Lhs == Rhs;
}

bool CShaderLibrary::SStringEqual::operator() (const std::string& Lhs, const std::string& Rhs) const noexcept
{
	return Lhs == Rhs;
}

bool CShaderLibrary::SStringEqual::operator() (const std::string& Lhs, std::string_view Rhs) const noexcept
{
	return std::string_view(Lhs) == Rhs;
}

bool CShaderLibrary::SStringEqual::operator() (std::string_view Lhs, const std::string& Rhs) const noexcept
{
	return Lhs == std::string_view(Rhs);
}

static std::string BuildShaderKey (std::string_view Name, const TArray<SShaderDefine>& Defines)
{
	if (Defines.IsEmpty())
	{
		return std::string(Name);
	}

	std::string key;
	key.reserve(Name.size() + Defines.Count() * 16u);
	key.append(Name);
	key.push_back('|');
	for (uint32 i = 0; i < Defines.Count(); ++i)
	{
		const SShaderDefine& define = Defines[i];
		if (define.Name.empty())
		{
			continue;
		}

		key.append(define.Name);
		if (!define.Value.empty())
		{
			key.push_back('=');
			key.append(define.Value);
		}
		key.push_back(';');
	}

	return key;
}

static std::filesystem::file_time_type GetLastWriteTime (const std::filesystem::path& Path)
{
	std::error_code error;
	const auto lastWriteTime = std::filesystem::last_write_time(Path, error);
	frt_assert(!error);
	return lastWriteTime;
}

static std::filesystem::path GetAbsolutePath (const std::filesystem::path& Path)
{
	if (Path.empty())
	{
		return {};
	}

	std::error_code error;
	const auto absolutePath = std::filesystem::absolute(Path, error);
	if (error)
	{
		return Path;
	}

	return absolutePath;
}

static bool WriteFileBytes (const std::filesystem::path& Path, const void* Data, size_t Size)
{
	if (Path.empty())
	{
		return false;
	}

	const std::filesystem::path absolutePath = GetAbsolutePath(Path);
	if (absolutePath.empty())
	{
		return false;
	}

	std::error_code error;
	std::filesystem::create_directories(absolutePath.parent_path(), error);
	if (error)
	{
		return false;
	}

 	FILE* file = nullptr;
	const std::string filePath = absolutePath.string();
	const int32 openResult = fopen_s(&file, filePath.c_str(), "wb");
	if (openResult != 0 || !file)
	{
		return false;
	}

	const size_t bytesWritten = Size > 0u ? fwrite(Data, 1, Size, file) : 0u;
	(void)fclose(file);
	return Size == 0u || bytesWritten == Size;
}

static bool CompileShader (SShaderAsset& Shader)
{
	if (Shader.SourcePath.empty())
	{
		return false;
	}

	if (Shader.TargetProfile.empty())
	{
		return false;
	}

	if (!IsLibraryTarget(Shader.TargetProfile) && Shader.EntryPoint.empty())
	{
		return false;
	}

#ifdef _WINDOWS
	const SDxcContext& context = GetDxcContext();
	if (!context.Compiler || !context.Library || !context.IncludeHandler)
	{
		return false;
	}

	const std::filesystem::path sourcePath = GetAbsolutePath(Shader.SourcePath);
	if (sourcePath.empty())
	{
		return false;
	}

	std::ifstream shaderFile(sourcePath);
	if (!shaderFile.good())
	{
		return false;
	}

	std::stringstream shaderStream;
	shaderStream << shaderFile.rdbuf();
	const std::string shaderSource = shaderStream.str();

	Microsoft::WRL::ComPtr<IDxcBlobEncoding> textBlob;
	const HRESULT createBlobHr = context.Library->CreateBlobWithEncodingFromPinned(
		reinterpret_cast<LPBYTE>(const_cast<char*>(shaderSource.data())),
		static_cast<uint32>(shaderSource.size()),
		0,
		textBlob.GetAddressOf());
	if (FAILED(createBlobHr) || !textBlob)
	{
		return false;
	}

	std::wstring sourcePathWide = sourcePath.wstring();
	std::wstring entryWide = ToWide(Shader.EntryPoint);
	std::wstring targetWide = ToWide(Shader.TargetProfile);
	if (entryWide.empty())
	{
		entryWide = L"";
	}

	const std::filesystem::path includePath =
		Shader.IncludeDir.empty() ? sourcePath.parent_path() : GetAbsolutePath(Shader.IncludeDir);

	TArray<std::wstring> argumentStrings;
	if (!includePath.empty())
	{
		argumentStrings.Add();
		argumentStrings[argumentStrings.Count() - 1] = L"-I";
		argumentStrings.Add();
		argumentStrings[argumentStrings.Count() - 1] = includePath.wstring();
	}
	argumentStrings.Add();
	argumentStrings[argumentStrings.Count() - 1] = L"-Zi";
	argumentStrings.Add();
	argumentStrings[argumentStrings.Count() - 1] = L"-Zpc";
	argumentStrings.Add();
	argumentStrings[argumentStrings.Count() - 1] = L"-Qembed_debug";

	TArray<LPCWSTR> arguments;
	for (uint32 i = 0; i < argumentStrings.Count(); ++i)
	{
		arguments.Add();
		arguments[arguments.Count() - 1] = argumentStrings[i].c_str();
	}

	TArray<std::wstring> defineNames;
	TArray<std::wstring> defineValues;
	TArray<DxcDefine> dxcDefines;
	for (uint32 i = 0; i < Shader.Defines.Count(); ++i)
	{
		const SShaderDefine& define = Shader.Defines[i];
		if (define.Name.empty())
		{
			continue;
		}

		defineNames.Add();
		defineNames[defineNames.Count() - 1] = ToWide(define.Name);
		defineValues.Add();
		defineValues[defineValues.Count() - 1] = ToWide(define.Value);

		DxcDefine dxcDefine = {};
		dxcDefine.Name = defineNames[defineNames.Count() - 1].c_str();
		dxcDefine.Value = defineValues[defineValues.Count() - 1].empty()
			? nullptr
			: defineValues[defineValues.Count() - 1].c_str();
		dxcDefines.Add(dxcDefine);
	}

	Microsoft::WRL::ComPtr<IDxcOperationResult> operationResult;
	const HRESULT compileHr = context.Compiler->Compile(
		textBlob.Get(),
		sourcePathWide.c_str(),
		entryWide.c_str(),
		targetWide.c_str(),
		arguments.IsEmpty() ? nullptr : arguments.GetData(),
		arguments.Count(),
		dxcDefines.IsEmpty() ? nullptr : dxcDefines.GetData(),
		dxcDefines.Count(),
		context.IncludeHandler.Get(),
		operationResult.GetAddressOf());
	if (FAILED(compileHr) || !operationResult)
	{
		return false;
	}

	HRESULT resultCode = E_FAIL;
	const HRESULT statusHr = operationResult->GetStatus(&resultCode);
	if (FAILED(statusHr) || FAILED(resultCode))
	{
		Microsoft::WRL::ComPtr<IDxcBlobEncoding> errorBlob;
		if (SUCCEEDED(operationResult->GetErrorBuffer(errorBlob.GetAddressOf())) && errorBlob)
		{
			std::string output(
				reinterpret_cast<const char*>(errorBlob->GetBufferPointer()),
				reinterpret_cast<const char*>(errorBlob->GetBufferPointer()) + errorBlob->GetBufferSize());
			if (!output.empty())
			{
				OutputDebugStringA(output.c_str());
			}
		}
		return false;
	}

	Microsoft::WRL::ComPtr<IDxcBlob> compiledBlob;
	const HRESULT resultHr = operationResult->GetResult(compiledBlob.GetAddressOf());
	if (FAILED(resultHr) || !compiledBlob)
	{
		return false;
	}

	const uint32 byteCount = static_cast<uint32>(compiledBlob->GetBufferSize());
	Shader.Bytecode.SetSizeUninitialized(byteCount);
	if (byteCount > 0u)
	{
		memcpy(Shader.Bytecode.GetData(), compiledBlob->GetBufferPointer(), byteCount);
	}

	if (!Shader.Path.empty())
	{
		(void)WriteFileBytes(Shader.Path, compiledBlob->GetBufferPointer(), compiledBlob->GetBufferSize());
	}

	if (IsLibraryTarget(Shader.TargetProfile))
	{
		Shader.DxcBlob = compiledBlob;
	}
	else
	{
		Shader.DxcBlob.Reset();
	}

	return !Shader.Bytecode.IsEmpty();
#else
	(void)Shader;
	return false;
#endif
}

static TArray<uint8> ReadFileBytes (const std::filesystem::path& Path)
{
	frt_assert(!Path.empty());

	const std::filesystem::path absolutePath = GetAbsolutePath(Path);
	const std::string pathString = absolutePath.string();
	FILE* file = nullptr;
	const int32 openResult = fopen_s(&file, pathString.c_str(), "rb");
	if (openResult != 0 || !file)
	{
		frt_assert(false);
		return {};
	}

	(void)fseek(file, 0, SEEK_END);
	const int64 fileSize = static_cast<int64>(ftell(file));
	(void)fseek(file, 0, SEEK_SET);

	frt_assert(fileSize >= 0);
	frt_assert(fileSize <= (std::numeric_limits<uint32>::max)());

	TArray<uint8> bytes;
	if (fileSize > 0)
	{
		const uint32 byteCount = static_cast<uint32>(fileSize);
		bytes.SetSizeUninitialized(byteCount);
		const uint32 bytesRead = static_cast<uint32>(
			fread(bytes.GetData(), 1, static_cast<size_t>(byteCount), file));
		frt_assert(bytesRead == byteCount);
	}

	(void)fclose(file);
	return bytes;
}


D3D12_SHADER_BYTECODE SShaderAsset::GetBytecode () const
{
	D3D12_SHADER_BYTECODE bytecode = {};
	bytecode.pShaderBytecode = Bytecode.IsEmpty() ? nullptr : Bytecode.GetData();
	bytecode.BytecodeLength = static_cast<SIZE_T>(Bytecode.GetSize());
	return bytecode;
}

IDxcBlob* SShaderAsset::GetDxcBlob () const
{
	return DxcBlob.Get();
}

bool SShaderAsset::ReloadIfChanged ()
{
	const std::filesystem::path watchPath = SourcePath.empty() ? Path : SourcePath;
	if (watchPath.empty())
	{
		return false;
	}

	const auto lastWriteTime = GetLastWriteTime(watchPath);
	if (lastWriteTime == LastWriteTime)
	{
		return false;
	}

	if (!SourcePath.empty())
	{
		if (!CompileShader(*this))
		{
			return false;
		}
	}
	else
	{
		TArray<uint8> newBytecode = ReadFileBytes(Path);
		if (newBytecode.IsEmpty())
		{
			return false;
		}
		Bytecode = std::move(newBytecode);
		DxcBlob.Reset();
	}

	LastWriteTime = lastWriteTime;
	return !Bytecode.IsEmpty();
}


const SShaderAsset* CShaderLibrary::LoadShader (
	std::string_view Name,
	const std::filesystem::path& CompiledPath,
	const std::filesystem::path& SourcePath,
	const std::filesystem::path& IncludeDir,
	std::string_view EntryPoint,
	std::string_view TargetProfile,
	EShaderStage Stage,
	const TArray<SShaderDefine>& Defines)
{
	frt_assert(!Name.empty());
	frt_assert(!CompiledPath.empty());

	const std::filesystem::path compiledPath = GetAbsolutePath(CompiledPath);
	const std::filesystem::path sourcePath = GetAbsolutePath(SourcePath);
	const std::filesystem::path includeDir = GetAbsolutePath(IncludeDir);

	const std::string key = BuildShaderKey(Name, Defines);
	auto it = Shaders.find(key);
	if (it != Shaders.end())
	{
		frt_assert(it->second.Path == compiledPath);
		frt_assert(it->second.SourcePath == sourcePath);
		frt_assert(it->second.IncludeDir == includeDir);
		if (!EntryPoint.empty())
		{
			frt_assert(it->second.EntryPoint == EntryPoint);
		}
		if (!TargetProfile.empty())
		{
			frt_assert(it->second.TargetProfile == TargetProfile);
		}
		frt_assert(it->second.Stage == Stage);
		return &it->second;
	}

	SShaderAsset asset;
	asset.Name = key;
	asset.Path = compiledPath;
	asset.SourcePath = sourcePath;
	asset.IncludeDir = includeDir;
	if (!EntryPoint.empty())
	{
		asset.EntryPoint = EntryPoint;
	}
	if (!TargetProfile.empty())
	{
		asset.TargetProfile = TargetProfile;
	}
	asset.Stage = Stage;
	asset.Defines = Defines;

	const bool bNeedsCompile = !asset.SourcePath.empty();
	if (bNeedsCompile)
	{
		const bool bCompiled = CompileShader(asset);
		if (!bCompiled || asset.Bytecode.IsEmpty())
		{
			if (!asset.Path.empty())
			{
				asset.Bytecode = ReadFileBytes(asset.Path);
			}
		}
		if (!IsLibraryTarget(asset.TargetProfile))
		{
			asset.DxcBlob.Reset();
		}
	}
	else
	{
		asset.Bytecode = ReadFileBytes(asset.Path);
		asset.DxcBlob.Reset();
	}

	frt_assert(!asset.Bytecode.IsEmpty());

	const std::filesystem::path watchPath = asset.SourcePath.empty() ? asset.Path : asset.SourcePath;
	asset.LastWriteTime = GetLastWriteTime(watchPath);
	frt_assert(!asset.Bytecode.IsEmpty());

	auto [insertedIt, inserted] = Shaders.emplace(asset.Name, std::move(asset));
	frt_assert(inserted);
	return &insertedIt->second;
}

const SShaderAsset* CShaderLibrary::GetShader (std::string_view Name) const
{
	frt_assert(!Name.empty());

	auto it = Shaders.find(Name);
	if (it == Shaders.end())
	{
		return nullptr;
	}

	return &it->second;
}

bool CShaderLibrary::ReloadModifiedShaders ()
{
	bool bAnyReloaded = false;
	for (auto& entry : Shaders)
	{
		if (entry.second.ReloadIfChanged())
		{
			bAnyReloaded = true;
		}
	}

	return bAnyReloaded;
}
}
