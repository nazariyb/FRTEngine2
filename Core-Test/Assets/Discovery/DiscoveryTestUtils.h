#pragma once

// Helpers of the discovery tests (mounts and the stat scan): a per-test tree in the temp directory with a
// non-ASCII name, file writers, and the two Windows-only fixtures the scan must cope with - a junction
// and a case-sensitive directory - each made in-process and skipped where this machine refuses. Names
// with a lone surrogate are written as WTF-8 ("\xED\xB0\x80" is U+DC00) and converted by ToPath.

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include "Containers/String.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Platform/Process.h"

#include <Windows.h>
#include <winioctl.h>


namespace frt::test::discovery
{
// The "\\?\<absolute>" form: names Win32 would otherwise reinterpret (a trailing dot, "aux.txt") reach the
// file system as they are. Path is absolute already: std::filesystem::absolute would strip a trailing dot.
inline std::filesystem::path ToVerbatim (const std::filesystem::path& Path)
{
	std::filesystem::path verbatim = FStringView(R"(\\?\)").ToPath();
	verbatim += Path.native();
	return verbatim.make_preferred();
}

// Creates (or truncates) a file under its exact name, through the verbatim path.
inline bool WriteVerbatimFile (const std::filesystem::path& Path, FStringView Bytes = "x")
{
	const HANDLE file = CreateFileW(ToVerbatim(Path).c_str(), GENERIC_WRITE, 0u, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	DWORD written = 0u;
	const bool bWritten = WriteFile(file, Bytes.GetData(), static_cast<DWORD>(Bytes.Len()), &written, nullptr) != 0;
	CloseHandle(file);
	return bWritten && written == Bytes.Len();
}

// Creates one directory under its exact name ("Trailing.", "aux"), through the verbatim path; its parent
// must exist. True when it exists afterwards.
inline bool MakeVerbatimDirectory (const std::filesystem::path& Path)
{
	return CreateDirectoryW(ToVerbatim(Path).c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

inline bool WriteText (const std::filesystem::path& Path, FStringView Text = "x")
{
	return platform::WriteFileBytes(Path, Text.AsBytes());
}

/**
 * Makes Link a junction to Target (an absolute directory): the mount-point reparse point "mklink /J"
 * makes, which needs no privilege. Link must not exist. False when this file system refuses.
 */
inline bool MakeJunction (const std::filesystem::path& Link, const std::filesystem::path& Target)
{
	if (!CreateDirectoryW(ToVerbatim(Link).c_str(), nullptr))
	{
		return false;
	}
	const HANDLE handle = CreateFileW(ToVerbatim(Link).c_str(), GENERIC_WRITE, 0u, nullptr, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
	{
		RemoveDirectoryW(ToVerbatim(Link).c_str());
		return false;
	}

	// REPARSE_DATA_BUFFER's MountPointReparseBuffer (ntifs.h, not in the user-mode headers): the NT path of
	// the target and the path to print, each NUL-terminated, in UTF-16 units.
	using FUnit = std::filesystem::path::value_type;
	std::filesystem::path substitute = FStringView(R"(\??\)").ToPath();
	substitute += Target.native();
	substitute.make_preferred();
	std::filesystem::path print = Target;
	print.make_preferred();
	const size_t substituteBytes = substitute.native().size() * sizeof(FUnit);
	const size_t printBytes = print.native().size() * sizeof(FUnit);
	const size_t pathBytes = substituteBytes + sizeof(FUnit) + printBytes + sizeof(FUnit);
	std::vector<uint8> buffer(8u + 8u + pathBytes, uint8(0));
	const auto put16 = [&buffer] (size_t Offset, size_t Value)
	{
		const uint16 value = static_cast<uint16>(Value);
		std::memcpy(buffer.data() + Offset, &value, sizeof(value));
	};
	const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
	std::memcpy(buffer.data(), &tag, sizeof(tag));
	put16(4u, 8u + pathBytes);                                  // ReparseDataLength
	put16(8u, 0u);                                              // SubstituteNameOffset
	put16(10u, substituteBytes);                                // SubstituteNameLength
	put16(12u, substituteBytes + sizeof(FUnit));                // PrintNameOffset
	put16(14u, printBytes);                                     // PrintNameLength
	std::memcpy(buffer.data() + 16u, substitute.c_str(), substituteBytes);
	std::memcpy(buffer.data() + 16u + substituteBytes + sizeof(FUnit), print.c_str(), printBytes);

	DWORD returned = 0u;
	const bool bSet = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0u,
		&returned, nullptr) != 0;
	CloseHandle(handle);
	if (!bSet)
	{
		RemoveDirectoryW(ToVerbatim(Link).c_str());
	}
	return bSet;
}

// Removes a junction itself, never what it points to.
inline void RemoveJunction (const std::filesystem::path& Link)
{
	RemoveDirectoryW(ToVerbatim(Link).c_str());
}

/**
 * Gives the empty directory Directory the per-directory case-sensitive attribute ("fsutil file
 * setCaseSensitiveInfo"), which directories created inside it inherit. Needs NTFS and, on some Windows
 * versions, the WSL feature: false where it cannot be set.
 */
inline bool MakeCaseSensitive (const std::filesystem::path& Directory)
{
	const HANDLE handle = CreateFileW(Directory.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	FILE_CASE_SENSITIVE_INFO caseInfo = {};
	caseInfo.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
	const bool bSet = SetFileInformationByHandle(handle, FileCaseSensitiveInfo, &caseInfo, sizeof(caseInfo)) != 0;
	CloseHandle(handle);
	return bSet;
}

// Deletes Root and everything below it, through verbatim paths (names Win32 cannot delete otherwise), and
// junctions as links. Best effort.
inline void RemoveTree (const std::filesystem::path& Root)
{
	std::vector<std::filesystem::path> directories;
	std::vector<std::filesystem::path> pending { Root };
	while (!pending.empty())
	{
		const std::filesystem::path directory = pending.back();
		pending.pop_back();
		directories.push_back(directory);
		// Listed verbatim too: "Trailing." listed as it is spelled would list "Trailing".
		platform::EnumerateDirectory(ToVerbatim(directory), [&] (const platform::SDirectoryEntry& Entry)
		{
			const std::filesystem::path path = directory / Entry.Name.ToPath();
			if (Entry.bReparsePoint && Entry.Stat.bDirectory)
			{
				RemoveJunction(path);
			}
			else if (Entry.Stat.bDirectory)
			{
				pending.push_back(path);
			}
			else
			{
				SetFileAttributesW(ToVerbatim(path).c_str(), FILE_ATTRIBUTE_NORMAL);
				DeleteFileW(ToVerbatim(path).c_str());
			}
		});
	}
	for (auto it = directories.rbegin(); it != directories.rend(); ++it)
	{
		RemoveDirectoryW(ToVerbatim(*it).c_str());
	}
}

/**
 * Every test gets <temp>/FRT Пошук 发现 😀 <pid>/<Name> (removed before and after), with the layout
 * Resolve expects: Engine/{Content,Config}, Проєкт 项目/{Content,Config,Plugins}.
 */
class CDiscoveryTree
{
public:
	explicit CDiscoveryTree (FStringView Name)
	{
		Base = std::filesystem::temp_directory_path() / FString::Format("FRT Пошук 发现 😀 {}", platform::GetCurrentProcessId()).ToPath();
		Root = Base / Name.ToPath();
		RemoveTree(Root);
		std::error_code ec;
		std::filesystem::create_directories(Root, ec);
	}

	~CDiscoveryTree ()
	{
		RemoveTree(Root);
		std::error_code ec;
		std::filesystem::remove(Base, ec);
	}

	CDiscoveryTree (const CDiscoveryTree&) = delete;
	CDiscoveryTree& operator= (const CDiscoveryTree&) = delete;

	[[nodiscard]] std::filesystem::path EngineDir () const { return Root / "Engine"; }
	[[nodiscard]] std::filesystem::path EngineContent () const { return EngineDir() / "Content"; }
	[[nodiscard]] std::filesystem::path EngineConfig () const { return EngineDir() / "Config"; }
	[[nodiscard]] std::filesystem::path ProjectDir () const { return Root / FStringView("Проєкт 项目").ToPath(); }
	[[nodiscard]] std::filesystem::path ProjectContent () const { return ProjectDir() / "Content"; }

	// Path below Root, '/' separated in Relative.
	[[nodiscard]] std::filesystem::path operator/ (FStringView Relative) const { return Root / Relative.ToPath(); }

	std::filesystem::path Base;
	std::filesystem::path Root;
};
}
