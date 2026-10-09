#include "Graphics/AssimpWideIO.h"

#include <cstdio>
#include <filesystem>
#include <share.h>
#include <span>
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

// _SH_DENYNO, as DefaultIOSystem's _wfopen: a model an exporter or a sync client still holds open
// for writing opens, and a program can save the file while it is read. (_wfopen_s denies writers.)
FILE* OpenShared (FStringView Name, const char* Mode)
{
	return _wfsopen(Name.ToPath().c_str(), ToWide(Mode).CStr(), _SH_DENYNO);
}
}

std::optional<FString> DecodeUriEscapes (FStringView Uri)
{
	FString decoded;
	uint32 copied = 0u;         // Uri[0, copied) is in decoded
	for (uint32 i = Uri.Find('%'); i != FStringView::NotFound; i = Uri.Find('%', i + 1u))
	{
		uint8 byte = 0u;
		if (!Uri.Mid(i + 1u, 2u).DecodeHex(std::span(&byte, 1u)))
		{
			continue;
		}
		if (byte == 0u)
		{
			return std::nullopt;
		}
		decoded.Append(Uri.Mid(copied, i - copied)).AppendChar(static_cast<char>(byte));
		i += 2u;
		copied = i + 1u;
	}

	if (copied == 0u)
	{
		return std::nullopt;
	}
	decoded.Append(Uri.RightChop(copied));
	return decoded;
}

bool CAssimpWideIOSystem::Exists (const char* Name) const
{
	if (Name == nullptr)
	{
		return false;
	}
	std::error_code ec;
	if (std::filesystem::exists(FStringView(Name).ToPath(), ec))
	{
		return true;
	}
	const std::optional<FString> decoded = DecodeUriEscapes(Name);
	return decoded && std::filesystem::exists(decoded->ToPath(), ec);
}

Assimp::IOStream* CAssimpWideIOSystem::Open (const char* Name, const char* Mode)
{
	if (Name == nullptr || *Name == '\0' || Mode == nullptr)
	{
		return nullptr;
	}

	// The name as written first: decoded, a file whose name holds an escape would be missed.
	FILE* file = OpenShared(Name, Mode);
	if (file == nullptr)
	{
		if (const std::optional<FString> decoded = DecodeUriEscapes(Name))
		{
			file = OpenShared(*decoded, Mode);
		}
	}
	if (file == nullptr)
	{
		return nullptr;
	}
	return new CAssimpWideStream(file, Name);
}

bool CAssimpWideIOSystem::CreateDirectory (const std::string& Path)
{
	std::error_code ec;
	return !Path.empty() && std::filesystem::create_directory(FStringView(Path).ToPath(), ec);
}

bool CAssimpWideIOSystem::DeleteFile (const std::string& File)
{
	std::error_code ec;
	return !File.empty() && std::filesystem::remove(FStringView(File).ToPath(), ec);
}
}
