#pragma once

#include <filesystem>

#include <yaml-cpp/yaml.h>

#include "Core.h"
#include "Containers/Array.h"
#include "Containers/String.h"
#include "Assets/Archive.h"


namespace frt::assets::yaml
{
// Read archive over a yaml-cpp Node tree.
// BeginGroup pushes the named child onto an internal cursor stack;
// reads target the top-of-stack node.
class FRT_CORE_API CYamlReadArchive : public IReadArchive
{
public:
	CYamlReadArchive () = default;

	bool LoadFromFile (const std::filesystem::path& Path);
	void SetRoot (YAML::Node Root);

	// IReadArchive
	bool Read (FStringLiteral Name, bool& V) override;
	bool Read (FStringLiteral Name, int32& V) override;
	bool Read (FStringLiteral Name, uint32& V) override;
	bool Read (FStringLiteral Name, float& V) override;
	bool Read (FStringLiteral Name, FString& V) override;
	bool Has (FStringLiteral Name) const override;
	bool BeginGroup (FStringLiteral Name) override;
	void EndGroup () override;

private:
	const YAML::Node& Top () const;

	YAML::Node RootNode;
	TArray<YAML::Node> Stack;
};


// Write archive that builds a yaml-cpp Node tree, then dumps it to file.
//
// Text yaml-cpp would alter is refused, not written: it replaces invalid UTF-8, the surrogates of
// WTF-8 path names and the noncharacters with U+FFFD in any value it decides to quote
// (assets::IsYamlStorable). The first refusal is kept in GetError(), and SaveToFile fails while
// there is one, so a file is never written with a value missing or changed.
class FRT_CORE_API CYamlWriteArchive : public IWriteArchive
{
public:
	CYamlWriteArchive ();

	bool SaveToFile (const std::filesystem::path& Path) const;
	const YAML::Node& GetRoot () const { return RootNode; }
	// Empty unless a value was refused.
	const FString& GetError () const { return Error; }

	// IWriteArchive
	using IWriteArchive::Write;
	void Write (FStringLiteral Name, bool V) override;
	void Write (FStringLiteral Name, int32 V) override;
	void Write (FStringLiteral Name, uint32 V) override;
	void Write (FStringLiteral Name, float V) override;
	void Write (FStringLiteral Name, FStringView V) override;
	void BeginGroup (FStringLiteral Name) override;
	void BeginGroupFlow (FStringLiteral Name) override;
	void EndGroup () override;

private:
	YAML::Node& Top ();

	YAML::Node RootNode;
	TArray<YAML::Node> Stack;
	FString Error;
};
}
