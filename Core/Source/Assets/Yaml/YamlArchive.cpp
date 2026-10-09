#include "Assets/Yaml/YamlArchive.h"

#include <optional>

#include "Asserts.h"
#include "Assets/Yaml/YamlString.h"
#include "Platform/Platform.h"


namespace frt::assets::yaml
{
// ---------------- CYamlReadArchive ----------------

bool CYamlReadArchive::LoadFromFile (const std::filesystem::path& Path)
{
	RootNode = YAML::Node();
	Stack.Clear();

	// Read through the UTF-16 path, then parsed as text: YAML::LoadFile takes the file name as
	// narrow text, which the CRT decodes with the process code page.
	const std::optional<FString> text = platform::ReadFileText(Path);
	if (!text)
	{
		return false;
	}

	try
	{
		RootNode = YAML::Load(text->ToStdString());
	}
	catch (const YAML::Exception&)
	{
		RootNode = YAML::Node();
		return false;
	}

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
static bool ReadAs (const YAML::Node& Node, FStringLiteral Name, T& OutValue)
{
	if (!Node.IsDefined() || !Node.IsMap())
	{
		return false;
	}

	const YAML::Node child = Node[Name.CStr()];
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

bool CYamlReadArchive::Read (FStringLiteral Name, bool& V)    { return ReadAs(Top(), Name, V); }
bool CYamlReadArchive::Read (FStringLiteral Name, int32& V)   { return ReadAs(Top(), Name, V); }
bool CYamlReadArchive::Read (FStringLiteral Name, uint32& V)  { return ReadAs(Top(), Name, V); }
bool CYamlReadArchive::Read (FStringLiteral Name, float& V)   { return ReadAs(Top(), Name, V); }
bool CYamlReadArchive::Read (FStringLiteral Name, FString& V) { return ReadAs(Top(), Name, V); }

bool CYamlReadArchive::Has (FStringLiteral Name) const
{
	const YAML::Node& top = Top();
	if (!top.IsDefined() || !top.IsMap())
	{
		return false;
	}
	const YAML::Node child = top[Name.CStr()];
	return child.IsDefined() && !child.IsNull();
}

bool CYamlReadArchive::BeginGroup (FStringLiteral Name)
{
	const YAML::Node& top = Top();
	if (!top.IsDefined() || !top.IsMap())
	{
		return false;
	}

	YAML::Node child = top[Name.CStr()];
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
	if (!Error.IsEmpty())
	{
		return false;
	}

	YAML::Emitter emitter;
	emitter << RootNode;
	// The emitter leaves the last line unterminated.
	return platform::WriteFileText(Path, FStringView(emitter.c_str(), emitter.size()) + "\n");
}

void CYamlWriteArchive::Write (FStringLiteral Name, bool V)   { Top()[Name.CStr()] = V; }
void CYamlWriteArchive::Write (FStringLiteral Name, int32 V)  { Top()[Name.CStr()] = V; }
void CYamlWriteArchive::Write (FStringLiteral Name, uint32 V) { Top()[Name.CStr()] = V; }
void CYamlWriteArchive::Write (FStringLiteral Name, float V)  { Top()[Name.CStr()] = V; }

void CYamlWriteArchive::Write (FStringLiteral Name, FStringView V)
{
	if (!IsYamlStorable(V))
	{
		if (Error.IsEmpty())
		{
			Error = FString::Format("{}: text cannot be stored in YAML (invalid UTF-8 or a noncharacter)", Name);
		}
		return;
	}
	Top()[Name.CStr()] = V.ToStdString();
}

void CYamlWriteArchive::BeginGroup (FStringLiteral Name)
{
	YAML::Node child(YAML::NodeType::Map);
	Top()[Name.CStr()] = child;
	// Re-fetch through the parent so the stored node is the actual subtree, not a detached copy.
	Stack.Add(Top()[Name.CStr()]);
}

void CYamlWriteArchive::BeginGroupFlow (FStringLiteral Name)
{
	YAML::Node child(YAML::NodeType::Map);
	child.SetStyle(YAML::EmitterStyle::Flow);
	Top()[Name.CStr()] = child;
	Stack.Add(Top()[Name.CStr()]);
}

void CYamlWriteArchive::EndGroup ()
{
	frt_assert(Stack.Count() > 1u);
	Stack.RemoveAt(Stack.Count() - 1u);
}
}
