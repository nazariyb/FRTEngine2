#pragma once

#include <filesystem>

#include <yaml-cpp/yaml.h>

#include "Core.h"
#include "Containers/Array.h"
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
	bool Read (const char* Name, bool& V) override;
	bool Read (const char* Name, int32& V) override;
	bool Read (const char* Name, uint32& V) override;
	bool Read (const char* Name, float& V) override;
	bool Read (const char* Name, std::string& V) override;
	bool Has (const char* Name) const override;
	bool BeginGroup (const char* Name) override;
	void EndGroup () override;

private:
	const YAML::Node& Top () const;

	YAML::Node RootNode;
	TArray<YAML::Node> Stack;
};


// Write archive that builds a yaml-cpp Node tree, then dumps it to file.
class FRT_CORE_API CYamlWriteArchive : public IWriteArchive
{
public:
	CYamlWriteArchive ();

	bool SaveToFile (const std::filesystem::path& Path) const;
	const YAML::Node& GetRoot () const { return RootNode; }

	// IWriteArchive
	void Write (const char* Name, bool V) override;
	void Write (const char* Name, int32 V) override;
	void Write (const char* Name, uint32 V) override;
	void Write (const char* Name, float V) override;
	void Write (const char* Name, const std::string& V) override;
	void BeginGroup (const char* Name) override;
	void EndGroup () override;

private:
	YAML::Node& Top ();

	YAML::Node RootNode;
	TArray<YAML::Node> Stack;
};
}
