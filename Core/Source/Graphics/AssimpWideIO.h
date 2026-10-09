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
 * escapes decode to one (DecodeUriEscapes) is opened decoded.
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

/**
 * Uri with its %XX escapes replaced by the bytes they stand for; a '%' without two hex digits after
 * it is kept. nullopt when there is no escape, or one stands for NUL (the name would end there).
 *
 * glTF names its buffers and images by URI, and exporters escape what a URI cannot hold (Blender
 * writes "Brick Wall.png" as "Brick%20Wall.png", "Текстура.png" as "%D0%A2%D0%B5..."). Assimp
 * passes the URI on as written: its own fallback (FileSystemFilter::Cleanup) means to decode it but
 * tests the '%' itself for a hex digit, so it never does. Callers decode only to find a file the
 * name as written does not name: a file called "Raw%41.png" is still found as itself.
 */
std::optional<FString> DecodeUriEscapes (FStringView Uri);
}
