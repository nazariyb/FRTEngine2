#pragma once

#include <assimp/DefaultIOSystem.h>


namespace frt::graphics
{
/**
 * Assimp's file access, opening files by their UTF-16 names.
 *
 * Assimp takes file names as narrow text. Engine paths reach it as UTF-8 (FString::FromPath), and
 * its own DefaultIOSystem converts them back with MultiByteToWideChar(CP_UTF8), which turns the
 * WTF-8 form of an unpaired surrogate (a legal Windows file name) into U+FFFD and fails to open the
 * file. This one decodes WTF-8 (FStringView::ToPath) and opens with _wfopen_s, so every name
 * Windows allows works - side files (.bin buffers, textures) resolve through it too.
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
};
}
