#include "Assets/Meta/Migrations.h"

#include <algorithm>
#include <iterator>
#include <optional>

#include "Assets/AssetDiagnosticCodes.h"
#include "Assets/Vcs/VcsProvider.h"
#include "Config/Config.h"
#include "Platform/FileSystem.h"
#include "Platform/Platform.h"
#include "Reflect/CanonicalEmitter.h"
#include "Reflect/DataTree.h"
#include "Reflect/DataTreeYaml.h"
#include "Unicode/DisplayText.h"


namespace frt::assets
{
namespace
{
// §14.8's order. `documents` runs whichever of its parts the engine knows (materials from M7, input from M10).
constexpr SMigrationStep Steps[] =
{
	{ "metas", "M4", true },
	{ "material-refs", "M7", false },
	{ "documents", "M7", false },
	{ "texture-colorspaces", "M8", false },
	{ "model-materials", "M9a", false },
};

constexpr FStringLiteral TopLevelKeys[] = { "FormatVersion", "Applied" };
constexpr reflect::SEmitRule TopLevelRule = { .Path = "", .KeyOrder = TopLevelKeys, .bKeyOrderOnly = true };

bool Refuse (SDiagnostic* OutError, FString Message, uint32 Line = 0u, uint32 Column = 0u)
{
	if (OutError != nullptr)
	{
		*OutError = SDiagnostic {};
		OutError->Severity = ESeverity::Error;
		OutError->Code = Mig001;
		OutError->File = FString(MigrationsFile);
		OutError->Line = Line;
		OutError->Column = Column;
		OutError->Message = std::move(Message);
	}
	return false;
}
}


std::span<const SMigrationStep> GetMigrationSteps () noexcept
{
	return Steps;
}

const SMigrationStep* FindMigrationStep (FStringView Name) noexcept
{
	for (const SMigrationStep& step : Steps)
	{
		if (Name.Equals<ESearchCase::IgnoreCaseAscii>(step.Name))
		{
			return &step;
		}
	}
	return nullptr;
}

std::filesystem::path GetMigrationsPath (const std::filesystem::path& Root)
{
	return Root / "Schemas" / "Migrations.yml";
}

bool ReadMigrations (const std::filesystem::path& Root, SMigrationsFile& Out, SDiagnostic* OutError)
{
	Out = SMigrationsFile {};
	const std::filesystem::path path = GetMigrationsPath(Root);
	const std::optional<platform::SFileStat> stat = platform::Stat(path);
	if (!stat.has_value())
	{
		return true;
	}
	Out.bExists = true;
	FString error;
	const std::optional<std::vector<uint8>> bytes = stat->bDirectory ? std::nullopt : platform::ReadFileBytesUncached(path, &error);
	if (!bytes.has_value())
	{
		return Refuse(OutError, FString::Format("{} cannot be read: {}", unicode::ToDisplayText(FString::FromPath(path)),
			error.IsEmpty() ? FStringView("a directory") : error.View()));
	}
	reflect::CDataTree tree;
	SDiagnostic parseError;
	if (!reflect::ParseYaml(FStringView(reinterpret_cast<const char*>(bytes->data()), static_cast<uint32>(bytes->size())), tree, &parseError))
	{
		return Refuse(OutError, FString::Format("{} is not YAML: {}", MigrationsFile, parseError.Message), parseError.Line, parseError.Column);
	}
	const reflect::HDataNode root = tree.GetRoot();
	if (!root.IsMap())
	{
		return Refuse(OutError, FString::Format("{} holds no map", MigrationsFile));
	}
	const reflect::HDataNode version = root.FindChild("FormatVersion");
	const std::optional<int64> number = version ? version.AsInt64() : std::nullopt;
	if (!number.has_value() || *number < 1)
	{
		return Refuse(OutError, FString::Format("{} has no FormatVersion", MigrationsFile), version ? version.GetLine() : 0u);
	}
	if (*number > static_cast<int64>(MigrationsFormatVersion))
	{
		Out.bNewerFormat = true;
		return Refuse(OutError, FString::Format("{} is FormatVersion {}, newer than this engine's {}: written by a newer engine",
			MigrationsFile, *number, MigrationsFormatVersion), version.GetLine());
	}
	const reflect::HDataNode applied = root.FindChild("Applied");
	if (!applied)
	{
		return true;
	}
	if (!applied.IsSequence())
	{
		return Refuse(OutError, FString::Format("{}: Applied is not a list of step names", MigrationsFile), applied.GetLine(), applied.GetColumn());
	}
	for (const reflect::SDataChild& item : applied.GetChildren())
	{
		const std::optional<FStringView> name = item.Value.IsScalar() ? item.Value.AsText() : std::nullopt;
		if (!name.has_value() || name->IsEmpty())
		{
			return Refuse(OutError, FString::Format("{}: Applied holds something that is no step name", MigrationsFile), item.Value.GetLine(),
				item.Value.GetColumn());
		}
		Out.Applied.emplace_back(*name);
	}
	return true;
}

bool EmitMigrations (std::span<const FString> Applied, FString& Out, SDiagnostic* OutError)
{
	std::vector<FString> names(Applied.begin(), Applied.end());
	std::sort(names.begin(), names.end(), [] (const FString& A, const FString& B) { return A.View() < B.View(); });
	names.erase(std::unique(names.begin(), names.end()), names.end());

	reflect::CDataTreeBuilder builder;
	builder.BeginMap(0u, 0u);
	builder.Key("FormatVersion", 0u, 0u);
	FString version;
	version.AppendNumber(MigrationsFormatVersion);
	builder.Scalar(version.View(), reflect::EScalarStyle::Plain, 0u, 0u);
	builder.Key("Applied", 0u, 0u);
	builder.BeginSequence(0u, 0u);
	for (const FString& name : names)
	{
		// Quoted in, so a name that spells a number stays text; the emitter drops quotes nothing needs.
		builder.Scalar(name.View(), reflect::EScalarStyle::DoubleQuoted, 0u, 0u);
	}
	builder.End();
	builder.End();
	reflect::CDataTree tree;
	if (!builder.Finish(tree, OutError))
	{
		return false;
	}
	const reflect::SEmitPolicy policy = { .Rules = std::span(&TopLevelRule, 1u) };
	return reflect::EmitCanonical(tree, policy, Out, OutError);
}

std::vector<FStringLiteral> GetMissingMigrations (const SMigrationsFile& File)
{
	std::vector<FStringLiteral> missing;
	for (const SMigrationStep& step : Steps)
	{
		const bool bApplied = std::any_of(File.Applied.begin(), File.Applied.end(),
			[&] (const FString& Name) { return Name.View().Equals<ESearchCase::IgnoreCaseAscii>(step.Name); });
		if (step.bAvailable && !bApplied)
		{
			missing.push_back(step.Name);
		}
	}
	return missing;
}

bool RecordMigration (const std::filesystem::path& Root, FStringView Step, IVcsProvider* Provider, FString* OutError)
{
	const auto fail = [&] (FString Message)
	{
		if (OutError != nullptr)
		{
			*OutError = std::move(Message);
		}
		return false;
	};
	SMigrationsFile file;
	SDiagnostic error;
	if (!ReadMigrations(Root, file, &error))
	{
		return fail(error.Message);
	}
	if (std::any_of(file.Applied.begin(), file.Applied.end(), [&] (const FString& Name) { return Name.View().Equals<ESearchCase::IgnoreCaseAscii>(Step); }))
	{
		return true;
	}
	file.Applied.emplace_back(Step);
	FString text;
	if (!EmitMigrations(file.Applied, text, &error))
	{
		return fail(error.Message);
	}
	const std::filesystem::path path = GetMigrationsPath(Root);
	if (file.bExists && Provider != nullptr)
	{
		const FString files[] = { FString::FromPath(path) };
		const SVcsResult checkedOut = Provider->CheckOut(files);
		if (checkedOut.Error != EVcsError::None)
		{
			return fail(checkedOut.Message);
		}
	}
	FString writeError;
	if (!platform::WriteFileAtomic(path, text.View(), true, &writeError))
	{
		return fail(FString::Format("cannot write {}: {}", unicode::ToDisplayText(FString::FromPath(path)), writeError));
	}
	return true;
}

std::optional<SDiagnostic> FindMigrationGap (const std::filesystem::path& Root, FStringView RootMountPath, bool bEngine,
	bool* OutNewerEngine)
{
	SMigrationsFile file;
	SDiagnostic error;
	FString message;
	const FStringView command = bEngine ? FStringView("FRT.bat assets migrate --all --engine") : FStringView("FRT.bat assets migrate --all");
	if (OutNewerEngine != nullptr)
	{
		*OutNewerEngine = false;
	}
	const bool bRead = ReadMigrations(Root, file, &error);
	if (!bRead && file.bNewerFormat)
	{
		// Ahead of this engine: nothing here can fix the file, and migrate cannot read it either.
		message = FString::Format("{} needs a newer engine: {}. Open it with that engine; this one writes nothing in it", RootMountPath,
			error.Message);
		if (OutNewerEngine != nullptr)
		{
			*OutNewerEngine = true;
		}
	}
	else if (!bRead)
	{
		message = FString::Format("{}: run {} after fixing it", error.Message, command);
	}
	else if (const std::vector<FStringLiteral> missing = GetMissingMigrations(file); !missing.empty())
	{
		FString steps;
		for (const FStringLiteral step : missing)
		{
			steps += steps.IsEmpty() ? "" : ", ";
			steps += step;
		}
		message = FString::Format("{} is behind this engine: {} {} not list {} (run {})", RootMountPath, MigrationsFile,
			file.bExists ? "does" : "does not exist, so it does", steps, command);
	}
	else
	{
		return std::nullopt;
	}
	SDiagnostic gap;
	gap.Severity = ESeverity::Error;
	gap.Code = Mig001;
	gap.Subject = FString(RootMountPath);
	gap.File = FString(MigrationsFile);
	gap.Line = error.Line;
	gap.Column = error.Column;
	gap.Message = std::move(message);
	return gap;
}

bool IsMigrationGateOn (const config::CConfig& Config)
{
	return Config.Get("Assets", "MigrationGate", MigrationGateDefault);
}
}
