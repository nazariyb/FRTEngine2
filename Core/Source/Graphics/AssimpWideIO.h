#pragma once

#include <optional>
#include <string>

#include <assimp/DefaultIOSystem.h>

#include "Containers/String.h"


namespace frt::graphics
{
/**
 * Assimp's file access, opening files by their UTF-16 names.
 *
 * Assimp takes file names as narrow text. Engine paths reach it as UTF-8 (FString::FromPath), and
 * its own DefaultIOSystem converts them back with MultiByteToWideChar(CP_UTF8), which turns the
 * WTF-8 form of an unpaired surrogate (a legal Windows file name) into U+FFFD and fails to open the
 * file. This one decodes WTF-8 (FStringView::ToPath) and opens with _wfsopen, so every name
 * Windows allows works - side files (.bin buffers, textures) resolve through it too. Files are
 * shared as DefaultIOSystem shares them (_SH_DENYNO). A name that names no file but whose URI
 * escapes decode to one (text::DecodeUriEscapes, Text/Uri.h) is opened decoded.
 *
 * Usage: Importer.SetIOHandler(new CAssimpWideIOSystem()); (the importer owns and deletes it),
 * then Importer.ReadFile(FString::FromPath(File).CStr(), Flags).
 *
 * Core only: Assimp's headers are on Core's include path alone, so nothing here is exported.
 */
class CAssimpWideIOSystem : public Assimp::DefaultIOSystem
{
public:
	bool Exists (const char* Name) const override;
	Assimp::IOStream* Open (const char* Name, const char* Mode = "rb") override;

	// Windows.h (CorePch.h) makes these names CreateDirectoryW / DeleteFileW in every engine source,
	// so this class's vtable would point at IOSystem functions Assimp's DLL does not export (it
	// exports them unrenamed). Overriding both keeps those slots in Core - by UTF-16 names, too.
	bool CreateDirectory (const std::string& Path) override;
	bool DeleteFile (const std::string& File) override;
};
}
