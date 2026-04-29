#include "Assets/Yaml/YamlArchive.h"

#include <fstream>

#include "Asserts.h"


namespace frt::assets::yaml
{
// ---------------- CYamlReadArchive ----------------

bool CYamlReadArchive::LoadFromFile (const std::filesystem::path& Path)
{
	try
	{
		RootNode = YAML::LoadFile(Path.string());
	}
	catch (const YAML::Exception&)
	{
		RootNode = YAML::Node();
		Stack.Clear();
		return false;
	}

	Stack.Clear();
	Stack.Add(RootNode);
	return RootNode.IsDefined() && !RootNode.IsNull();
}

void CYamlReadArchive::SetRoot (YAML::Node Root)
{
	RootNode = Root;
	Stack.Clear();
	Stack.Add(RootNode);
}

const YAML::Node& CYamlReadArchive::Top () const
{
	frt_assert(!Stack.IsEmpty());
	return Stack[Stack.Count() - 1u];
}

template <typename T>
static bool ReadAs (const YAML::Node& Node, const char* Name, T& OutValue)
{
	if (!Node.IsDefined() || !Node.IsMap())
	{
		return false;
	}

	const YAML::Node child = Node[Name];
	if (!child.IsDefined() || child.IsNull())
	{
		return false;
	}

	try
	{
		OutValue = child.as<T>();
		return true;
	}
	catch (const YAML::Exception&)
	{
		return false;
	}
}

bool CYamlReadArchive::Read (const char* Name, bool& V)        { return ReadAs(Top(), Name, V); }
bool CYamlReadArchive::Read (const char* Name, int32& V)       { return ReadAs(Top(), Name, V); }
bool CYamlReadArchive::Read (const char* Name, uint32& V)      { return ReadAs(Top(), Name, V); }
bool CYamlReadArchive::Read (const char* Name, float& V)       { return ReadAs(Top(), Name, V); }
bool CYamlReadArchive::Read (const char* Name, std::string& V) { return ReadAs(Top(), Name, V); }

bool CYamlReadArchive::Has (const char* Name) const
{
	const YAML::Node& top = Top();
	if (!top.IsDefined() || !top.IsMap())
	{
		return false;
	}
	const YAML::Node child = top[Name];
	return child.IsDefined() && !child.IsNull();
}

bool CYamlReadArchive::BeginGroup (const char* Name)
{
	const YAML::Node& top = Top();
	if (!top.IsDefined() || !top.IsMap())
	{
		return false;
	}

	YAML::Node child = top[Name];
	if (!child.IsDefined() || child.IsNull() || !child.IsMap())
	{
		return false;
	}

	Stack.Add(child);
	return true;
}

void CYamlReadArchive::EndGroup ()
{
	frt_assert(Stack.Count() > 1u);
	Stack.RemoveAt(Stack.Count() - 1u);
}


// ---------------- CYamlWriteArchive ----------------

CYamlWriteArchive::CYamlWriteArchive ()
{
	RootNode = YAML::Node(YAML::NodeType::Map);
	Stack.Add(RootNode);
}

YAML::Node& CYamlWriteArchive::Top ()
{
	frt_assert(!Stack.IsEmpty());
	return Stack[Stack.Count() - 1u];
}

bool CYamlWriteArchive::SaveToFile (const std::filesystem::path& Path) const
{
	std::error_code ec;
	std::filesystem::create_directories(Path.parent_path(), ec);

	std::ofstream stream(Path);
	if (!stream.is_open())
	{
		return false;
	}

	YAML::Emitter emitter;
	emitter << RootNode;
	stream << emitter.c_str() << "\n";
	return true;
}

void CYamlWriteArchive::Write (const char* Name, bool V)        { Top()[Name] = V; }
void CYamlWriteArchive::Write (const char* Name, int32 V)       { Top()[Name] = V; }
void CYamlWriteArchive::Write (const char* Name, uint32 V)      { Top()[Name] = V; }
void CYamlWriteArchive::Write (const char* Name, float V)       { Top()[Name] = V; }
void CYamlWriteArchive::Write (const char* Name, const std::string& V) { Top()[Name] = V; }

void CYamlWriteArchive::BeginGroup (const char* Name)
{
	YAML::Node child(YAML::NodeType::Map);
	Top()[Name] = child;
	// Re-fetch through the parent so the stored node is the actual subtree, not a detached copy.
	Stack.Add(Top()[Name]);
}

void CYamlWriteArchive::BeginGroupFlow (const char* Name)
{
	YAML::Node child(YAML::NodeType::Map);
	child.SetStyle(YAML::EmitterStyle::Flow);
	Top()[Name] = child;
	Stack.Add(Top()[Name]);
}

void CYamlWriteArchive::EndGroup ()
{
	frt_assert(Stack.Count() > 1u);
	Stack.RemoveAt(Stack.Count() - 1u);
}
}
