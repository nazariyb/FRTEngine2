#include "ShaderAsset.h"

#include <cstdio>
#include <limits>
#include <system_error>
#include <utility>

#ifdef _WINDOWS
#include <Windows.h>
#endif

#include "Asserts.h"


namespace frt::graphics
{
#ifdef _WINDOWS
static std::wstring ToWide (std::string_view Value)
{
	return std::wstring(Value.begin(), Value.end());
}

static std::wstring QuoteArgument (const std::wstring& Value)
{
	if (Value.find_first_of(L" \t\"") == std::wstring::npos)
	{
		return Value;
	}

	std::wstring escaped;
	escaped.reserve(Value.size() + 2);
	escaped.push_back(L'"');
	for (wchar_t ch : Value)
	{
		if (ch == L'"')
		{
			escaped.push_back(L'\\');
		}
		escaped.push_back(ch);
	}
	escaped.push_back(L'"');
	return escaped;
}

static bool RunProcess (const std::wstring& ApplicationPath, const std::wstring& CommandLine, std::string* OutOutput)
{
	SECURITY_ATTRIBUTES securityAttributes = {};
	securityAttributes.nLength = sizeof(securityAttributes);
	securityAttributes.bInheritHandle = TRUE;

	HANDLE outputRead = nullptr;
	HANDLE outputWrite = nullptr;
	if (!CreatePipe(&outputRead, &outputWrite, &securityAttributes, 0))
	{
		return false;
	}

	if (!SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0))
	{
		CloseHandle(outputWrite);
		CloseHandle(outputRead);
		return false;
	}

	STARTUPINFOW startupInfo = {};
	startupInfo.cb = sizeof(startupInfo);
	startupInfo.dwFlags = STARTF_USESTDHANDLES;
	startupInfo.hStdOutput = outputWrite;
	startupInfo.hStdError = outputWrite;

	PROCESS_INFORMATION processInfo = {};
	std::wstring mutableCommandLine = CommandLine;
	const BOOL created = CreateProcess(
		ApplicationPath.empty() ? nullptr : ApplicationPath.c_str(),
		mutableCommandLine.data(),
		nullptr,
		nullptr,
		TRUE,
		CREATE_NO_WINDOW,
		nullptr,
		nullptr,
		&startupInfo,
		&processInfo);

	CloseHandle(outputWrite);

	if (!created)
	{
		CloseHandle(outputRead);
		return false;
	}

	if (OutOutput)
	{
		char buffer[4096];
		DWORD bytesRead = 0;
		while (ReadFile(outputRead, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
		{
			OutOutput->append(buffer, buffer + bytesRead);
		}
	}

	CloseHandle(outputRead);

	WaitForSingleObject(processInfo.hProcess, INFINITE);

	DWORD exitCode = 1;
	(void)GetExitCodeProcess(processInfo.hProcess, &exitCode);

	CloseHandle(processInfo.hThread);
	CloseHandle(processInfo.hProcess);

	return exitCode == 0;
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

static std::filesystem::path FindDxcPath (const std::filesystem::path& SourcePath)
{
	std::filesystem::path searchPath = GetAbsolutePath(SourcePath);
	if (searchPath.has_filename())
	{
		searchPath = searchPath.parent_path();
	}

	for (int32 i = 0; i < 6 && !searchPath.empty(); ++i)
	{
		const std::filesystem::path candidate =
			searchPath / "ThirdParty" / "Dxc" / "bin" / "x64" / "dxc.exe";

		std::error_code error;
		const bool exists = std::filesystem::exists(candidate, error);
		if (!error && exists)
		{
			return candidate;
		}

		searchPath = searchPath.parent_path();
	}

	return {};
}

static bool CompileShader (const SShaderAsset& Shader)
{
	if (Shader.SourcePath.empty() || Shader.Path.empty())
	{
		return false;
	}

	if (Shader.EntryPoint.empty() || Shader.TargetProfile.empty())
	{
		return false;
	}

	const std::filesystem::path dxcPath = FindDxcPath(Shader.SourcePath);
	frt_assert(!dxcPath.empty());
	if (dxcPath.empty())
	{
		return false;
	}

#ifdef _WINDOWS
	const std::filesystem::path sourcePath = GetAbsolutePath(Shader.SourcePath);
	const std::filesystem::path outputPath = GetAbsolutePath(Shader.Path);
	const std::filesystem::path includeDir =
		Shader.IncludeDir.empty() ? sourcePath.parent_path() : GetAbsolutePath(Shader.IncludeDir);

	if (!outputPath.empty())
	{
		std::error_code error;
		std::filesystem::create_directories(outputPath.parent_path(), error);
	}

	std::wstring commandLine;
	commandLine.reserve(512);
	commandLine.append(QuoteArgument(dxcPath.wstring()));
	commandLine.append(L" -E ").append(ToWide(Shader.EntryPoint));
	commandLine.append(L" -Fo ").append(QuoteArgument(outputPath.wstring()));
	commandLine.append(L" -T ").append(ToWide(Shader.TargetProfile));
	commandLine.append(L" -Zi -Zpc -Qembed_debug");
	if (!includeDir.empty())
	{
		commandLine.append(L" -I ").append(QuoteArgument(includeDir.wstring()));
	}
	commandLine.append(L" ").append(QuoteArgument(sourcePath.wstring()));

	std::string output;
	const bool success = RunProcess(dxcPath.wstring(), commandLine, &output);
	if (!success && !output.empty())
	{
		OutputDebugStringA(output.c_str());
	}
	return success;
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
	fileSize <= std::numeric_limits<int64>::max();

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

	TArray<uint8> newBytecode = ReadFileBytes(Path);
	if (newBytecode.IsEmpty())
	{
		return false;
	}

	LastWriteTime = lastWriteTime;
	Bytecode = std::move(newBytecode);
	return !Bytecode.IsEmpty();
}


const SShaderAsset* CShaderLibrary::LoadShader (
	std::string_view Name,
	const std::filesystem::path& CompiledPath,
	const std::filesystem::path& SourcePath,
	const std::filesystem::path& IncludeDir,
	std::string_view EntryPoint,
	std::string_view TargetProfile,
	EShaderStage Stage)
{
	frt_assert(!Name.empty());
	frt_assert(!CompiledPath.empty());

	const std::filesystem::path compiledPath = GetAbsolutePath(CompiledPath);
	const std::filesystem::path sourcePath = GetAbsolutePath(SourcePath);
	const std::filesystem::path includeDir = GetAbsolutePath(IncludeDir);

	auto it = Shaders.find(Name);
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
	asset.Name = std::string(Name);
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

	const bool bNeedsCompile = !asset.SourcePath.empty();
	if (bNeedsCompile)
	{
		(void)CompileShader(asset);
	}

	asset.Bytecode = ReadFileBytes(asset.Path);
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
