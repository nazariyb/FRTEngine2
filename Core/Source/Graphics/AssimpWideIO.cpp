#include "Graphics/AssimpWideIO.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include <assimp/DefaultIOStream.h>

#include "Containers/String.h"
#include "Containers/WideString.h"


namespace frt::graphics
{
namespace
{
// Assimp's DefaultIOStream reads, seeks and closes the FILE*; only its (FILE*, name) constructor is
// protected, so this re-exposes it. Created and deleted through Assimp's heap (IOStream's operator
// new/delete), and the FILE* is closed by Assimp's CRT, which is the engine's: one CRT DLL in Debug
// and Development, one static CRT in the Release executable.
class CAssimpWideStream final : public Assimp::DefaultIOStream
{
public:
	CAssimpWideStream (FILE* File, const char* Name)
		: Assimp::DefaultIOStream(File, std::string(Name))
	{}
};
}

bool CAssimpWideIOSystem::Exists (const char* Name) const
{
	if (Name == nullptr)
	{
		return false;
	}
	std::error_code ec;
	return std::filesystem::exists(FStringView(Name).ToPath(), ec);
}

Assimp::IOStream* CAssimpWideIOSystem::Open (const char* Name, const char* Mode)
{
	if (Name == nullptr || *Name == '\0' || Mode == nullptr)
	{
		return nullptr;
	}

	FILE* file = nullptr;
	if (_wfopen_s(&file, FStringView(Name).ToPath().c_str(), ToWide(Mode).CStr()) != 0 || file == nullptr)
	{
		return nullptr;
	}
	return new CAssimpWideStream(file, Name);
}
}
