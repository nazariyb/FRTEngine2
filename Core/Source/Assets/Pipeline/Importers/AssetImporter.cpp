#include "Assets/Pipeline/Importers/AssetImporter.h"

#include <type_traits>
#include <utility>

#include "Asserts.h"
#include "BuildConfig.h"
#include "Assets/Pipeline/Importers/ImporterUtils.h"
#include "Platform/Platform.h"


namespace frt::assets
{
namespace importers
{
namespace
{
/**
 * Raw (fonts, the Raw lists of the rule files): the file is its own payload, so Analyze has nothing to read and
 * records no tags (§7.11). Its record exists so the cook can find the file and references can resolve to it.
 */
class CRawImporter final : public IAssetImporter
{
public:
	const SImporterDesc& GetInfo () const noexcept override
	{
		static const SImporterDesc* const desc = FindImporter("Raw");
		return *desc;
	}

	bool Analyze (const SAnalyzeInput& /*In*/, const SResolvedSettings& /*Settings*/, SAnalyzeResult& /*Out*/) const override
	{
		return true;
	}
};
}

const IAssetImporter& GetRawImporter () noexcept
{
	static const CRawImporter importer;
	return importer;
}

SDiagnostic MakeFileDiagnostic (const SAnalyzeInput& In, ESeverity Severity, FString Message, uint32 Code)
{
	SDiagnostic diagnostic;
	diagnostic.Severity = Severity;
	diagnostic.Code = Code;
	diagnostic.Subject = In.FileMountPath;
	diagnostic.File = In.SourceFile;
	diagnostic.Message = std::move(Message);
	return diagnostic;
}

std::optional<std::vector<uint8>> ReadWholeFileHashed (const SAnalyzeInput& In, SAnalyzeResult& Out)
{
	std::vector<uint8> bytes;
	hash::CContentHasher hasher;
	const platform::EStableRead read = platform::ReadFileChunksStable(In.Path, [&] (std::span<const uint8> Chunk)
	{
		bytes.insert(bytes.end(), Chunk.begin(), Chunk.end());
		hasher.Update(Chunk);
	});
	if (read == platform::EStableRead::Stable)
	{
		Out.ContentHash = hasher.Finalize();
		return bytes;
	}
	if (read == platform::EStableRead::Changed)
	{
		Out.bSourceChanged = true;
		Out.Diagnostics.push_back(MakeFileDiagnostic(In, ESeverity::Error,
			FString::Format("the file changed while it was read ({}): analyze it again once it settles", FString::FromPath(In.Path))));
	}
	else
	{
		Out.Diagnostics.push_back(MakeFileDiagnostic(In, ESeverity::Error, FString::Format("cannot read {}", FString::FromPath(In.Path))));
	}
	return std::nullopt;
}

std::optional<FString> DecodeSourceText (std::vector<uint8>&& Bytes, const SAnalyzeInput& In, SAnalyzeResult& Out)
{
	FString raw(FStringView(reinterpret_cast<const char*>(Bytes.data()), static_cast<uint32>(Bytes.size())));
	Bytes = {};
	std::optional<FString> text = platform::DecodeTextFile(std::move(raw));
	if (!text)
	{
		Out.Diagnostics.push_back(MakeFileDiagnostic(In, ESeverity::Error, "the file starts with a UTF-16 byte order mark but has an odd number of bytes"));
	}
	return text;
}

std::optional<FString> ResolveRelativeReference (FStringView FileMountPath, FStringView Reference, EReferenceProblem* OutProblem)
{
	return ResolveFolderReference(GetMountPathParent(FileMountPath), Reference, OutProblem);
}

std::optional<FString> ResolveFolderReference (FStringView FolderMountPath, FStringView Reference, EReferenceProblem* OutProblem)
{
	const auto fail = [OutProblem] (EReferenceProblem Problem) -> std::optional<FString>
	{
		if (OutProblem != nullptr)
		{
			*OutProblem = Problem;
		}
		return std::nullopt;
	};
	if (Reference.IsEmpty() || Reference[0] == '/' || Reference[0] == '\\' || Reference.Contains(':'))
	{
		return fail(EReferenceProblem::NotRelative);
	}
	std::optional<FString> path = ResolveMountPath(FolderMountPath, Reference);
	if (!path)
	{
		return fail(EReferenceProblem::LeavesMount);
	}
	if (OutProblem != nullptr)
	{
		*OutProblem = EReferenceProblem::None;
	}
	return path;
}

FStringLiteral DescribeReferenceProblem (EReferenceProblem Problem) noexcept
{
	switch (Problem)
	{
	case EReferenceProblem::NotRelative: return "it is not a path relative to the file's folder (an absolute path, a drive or a URI scheme)";
	case EReferenceProblem::LeavesMount: return "it climbs above the mount root";
	default:                             return "it names no file";
	}
}
}


namespace
{
// ImporterTable's available rows, in its order: GetAssetImporters()[i].GetInfo() is GetImporters()[i].
const IAssetImporter* const* GetImporterList () noexcept
{
	static const IAssetImporter* const list[] = {
		&importers::GetTextureImporter(),
		&importers::GetGltfImporter(),
		&importers::GetLegacyMaterialImporter(),
		&importers::GetLegacyInputImporter(),
		&importers::GetRawImporter(),
	};
	return list;
}

constexpr uint32 ImporterCount = 5u;

// Fills what the conventions of SAnalyzeResult ask and an importer may leave out.
void CompleteDiagnostics (const SAnalyzeInput& In, SAnalyzeResult& Out)
{
	for (SDiagnostic& diagnostic : Out.Diagnostics)
	{
		if (diagnostic.Subject.IsEmpty())
		{
			diagnostic.Subject = In.FileMountPath;
		}
		if (diagnostic.File.IsEmpty())
		{
			diagnostic.File = In.SourceFile;
		}
	}
}

#if FRT_BUILD_DEBUG
// An importer emits exactly the keys its type declares (AssetTypes.h): a key outside the table would be dropped
// or mistyped by the store.
void CheckTags (reflect::TypeId Type, const std::vector<STagDecl>& Tags)
{
	for (const STagDecl& tag : Tags)
	{
		const std::optional<uint32> key = CAssetTypeRegistry::FindKey(Type, tag.Key);
		frt_assert(key.has_value() && CAssetTypeRegistry::Get(Type).Keys[*key].Type == tag.Value.Type);
	}
}
#endif

template <typename TResult>
void AppendRecordUpsertImpl (TResult&& Result, const SEffectiveFileSettings& Settings, FStringView FileMountPath, SRegistryDelta& Delta)
{
	constexpr bool bMove = !std::is_lvalue_reference_v<TResult>;
	const auto take = [] (auto& Member) -> decltype(auto)
	{
		if constexpr (bMove)
		{
			return std::move(Member);
		}
		else
		{
			return static_cast<const std::remove_reference_t<decltype(Member)>&>(Member);
		}
	};

	SRecordUpsert& record = Delta.RecordUpserts.emplace_back();
	record.FileMountPath = FString(FileMountPath);
	record.Guid = MakeProvisionalGuid(FileMountPath);
	record.Type = Result.Type;
	if (Settings.Importer != nullptr)
	{
		record.Importer = FString(Settings.Importer->Name);
		if (Result.Status == EImportStatus::Analyzed || Result.Status == EImportStatus::AnalyzeFailed)
		{
			record.AnalyzeStamp = MakeAnalyzeStamp(Settings.Importer->AnalyzeVersion, Settings.Settings.AnalyzeHash);
		}
	}
	record.Flags = Result.Flags;
	record.CookRule = Settings.CookRule;
	record.Chunk = Settings.Chunk;
	record.RuleLabels = Settings.RuleLabels;
	if ((Result.Flags & ERecordFlags::Primary) != ERecordFlags::None)
	{
		record.PrimaryName = Result.PrimaryName;
	}
	record.ImportStatus = Result.Status;
	record.Tags = take(Result.Tags);
	record.SubEntries = take(Result.SubEntries);
	record.Edges = take(Result.Edges);
	record.Companions = take(Result.Companions);

	Delta.ProblemClears.push_back(SProblemsClear { FString(FileMountPath), EProblemSource::Analyze });
	// One set per subject, subjects in their first diagnostic's order, diagnostics in theirs.
	const size_t firstSet = Delta.Problems.size();
	for (auto& diagnostic : Result.Diagnostics)
	{
		SProblemsSet* set = nullptr;
		for (size_t i = firstSet; i < Delta.Problems.size(); ++i)
		{
			if (Delta.Problems[i].Subject == diagnostic.Subject)
			{
				set = &Delta.Problems[i];
				break;
			}
		}
		if (set == nullptr)
		{
			set = &Delta.Problems.emplace_back();
			set->Subject = diagnostic.Subject;
			set->Source = EProblemSource::Analyze;
		}
		set->Diagnostics.push_back(take(diagnostic));
	}
}
}


std::optional<SAnalyzeInput> SAnalyzeInput::FromMount (const SMount& Mount, FStringView FileMountPath)
{
	FStringView mountName;
	FStringView relative;
	if (!IsWellFormedMountPath(FileMountPath) || !SplitMountPath(FileMountPath, mountName, relative) || relative.IsEmpty()
		|| mountName != Mount.Name.View())
	{
		return std::nullopt;
	}
	SAnalyzeInput input;
	input.FileMountPath = FString(FileMountPath);
	input.Path = Mount.ContentDir / relative.ToPath();
	if (std::optional<FString> source = platform::GetRelativePath(input.Path, Mount.OwnerRoot, Mount.Volume.bCaseSensitive))
	{
		input.SourceFile = std::move(*source);
	}
	return input;
}

std::span<const IAssetImporter* const> GetAssetImporters () noexcept
{
	return std::span<const IAssetImporter* const>(GetImporterList(), ImporterCount);
}

const IAssetImporter* FindAssetImporter (const SImporterDesc& Importer) noexcept
{
	for (const IAssetImporter* importer : GetAssetImporters())
	{
		if (&importer->GetInfo() == &Importer)
		{
			return importer;
		}
	}
	return nullptr;
}

const IAssetImporter* FindAssetImporter (FStringView Name) noexcept
{
	const SImporterDesc* desc = FindImporter(Name);
	return desc != nullptr ? FindAssetImporter(*desc) : nullptr;
}

bool AnalyzeFile (const SAnalyzeInput& In, const SEffectiveFileSettings& Settings, SAnalyzeResult& Out)
{
	Out.Reset();
	if (Settings.Importer == nullptr)
	{
		return false;
	}
	Out.Type = Settings.Importer->GetMainType(GetMountPathLeaf(In.FileMountPath));

	const IAssetImporter* importer = FindAssetImporter(*Settings.Importer);
	if (importer == nullptr)
	{
		Out.Status = EImportStatus::ImporterUnavailable;
		Out.Diagnostics.push_back(importers::MakeFileDiagnostic(In, ESeverity::Info,
			FString::Format("the importer {} is not available in this build", Settings.Importer->Name)));
		return false;
	}

	const bool bAnalyzed = importer->Analyze(In, Settings.Settings, Out);
	Out.Status = bAnalyzed ? EImportStatus::Analyzed : EImportStatus::AnalyzeFailed;
	if (!bAnalyzed)
	{
		bool bReported = false;
		for (const SDiagnostic& diagnostic : Out.Diagnostics)
		{
			bReported = bReported || diagnostic.Severity != ESeverity::Info;
		}
		if (!bReported)
		{
			Out.Diagnostics.push_back(importers::MakeFileDiagnostic(In, ESeverity::Error,
				FString::Format("the {} importer could not analyze the file", Settings.Importer->Name)));
		}
	}
	CompleteDiagnostics(In, Out);
#if FRT_BUILD_DEBUG
	CheckTags(Out.Type, Out.Tags);
	for (const SSubEntryDecl& sub : Out.SubEntries)
	{
		CheckTags(sub.Type, sub.Tags);
	}
#endif
	return bAnalyzed;
}

void AppendRecordUpsert (const SAnalyzeResult& Result, const SEffectiveFileSettings& Settings, FStringView FileMountPath, SRegistryDelta& Delta)
{
	AppendRecordUpsertImpl(Result, Settings, FileMountPath, Delta);
}

void AppendRecordUpsert (SAnalyzeResult&& Result, const SEffectiveFileSettings& Settings, FStringView FileMountPath, SRegistryDelta& Delta)
{
	AppendRecordUpsertImpl(std::move(Result), Settings, FileMountPath, Delta);
}
}
