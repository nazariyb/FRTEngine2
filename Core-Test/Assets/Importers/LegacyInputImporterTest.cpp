// The Input.Legacy importer's Analyze (E3, design §14.4): `.frtinputmap` and `.frtinputaction` are line formats (they
// repeat `action:` and `bind:`), read as the runtime reads them - an input map gets the Actions tag, one Path edge per
// action to the action file CInputActionLibrary loads for it in the map's mount, and the Primary flag with its stem as
// PrimaryName; an action gets ValueType, the runtime's default Button when no `type:` parses.
#include <gtest/gtest.h>

#include <vector>

#include "ImporterTestUtils.h"

#include "Assets/FileHash.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::test::importers;


namespace
{
std::vector<FString> GetNameTags (const SAnalyzeResult& Result, FStringView Key)
{
	std::vector<FString> values;
	for (const STagDecl& tag : Result.Tags)
	{
		if (tag.Key == Key)
		{
			EXPECT_EQ(tag.Value.Type, ETagType::Name);
			values.push_back(tag.Value.Text);
		}
	}
	return values;
}
}


TEST(LegacyInputImporterTest, DemoInputMapDeclaresItsActions)
{
	const CMountTable mounts = ResolveDemoMounts();
	const SAnalyzeResult result = AnalyzeAt(mounts, "/Project/Input/IAM_Editor.frtinputmap");
	ASSERT_EQ(result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(result).CStr();
	EXPECT_TRUE(result.Diagnostics.empty()) << DescribeDiagnostics(result).CStr();
	EXPECT_EQ(result.Type, asset_types::InputActionMap);
	EXPECT_EQ(GetNameTags(result, "Actions"), (std::vector<FString> { "IA_EnableMove", "IA_MoveForward", "IA_MoveLeft", "IA_MoveUp" }));
	EXPECT_TRUE((result.Flags & ERecordFlags::Primary) != ERecordFlags::None);
	EXPECT_EQ(result.PrimaryName, "IAM_Editor");

	ASSERT_EQ(result.Edges.size(), 4u);
	const SEdgeDecl* edge = FindEdgeTo(result, "/Project/Input/IA_MoveForward.frtinputaction");
	ASSERT_NE(edge, nullptr);
	EXPECT_EQ(edge->Flags, EDepFlags::Path);
	EXPECT_EQ(edge->FieldPath, "actions.1.action");
	EXPECT_EQ(edge->FromSub, 0u);
	ASSERT_TRUE(result.ContentHash.has_value());

	const SAnalyzeResult action = AnalyzeAt(mounts, "/Project/Input/IA_MoveForward.frtinputaction");
	ASSERT_EQ(action.Status, EImportStatus::Analyzed) << DescribeDiagnostics(action).CStr();
	EXPECT_EQ(action.Type, asset_types::InputAction);
	EXPECT_EQ(GetNameTags(action, "ValueType"), (std::vector<FString> { "Button" }));
	EXPECT_TRUE(action.Edges.empty());
	EXPECT_TRUE((action.Flags & ERecordFlags::Primary) == ERecordFlags::None);
	EXPECT_TRUE(action.PrimaryName.IsEmpty());
}

TEST(LegacyInputImporterTest, MapNamesResolveAsTheActionLibraryBuildsPaths)
{
	const CImporterTree tree("MapNames");
	tree.Write("Controls/Gameplay/IAM_Пілот.frtinputmap",
		"version: 1\n"
		"type: Axis\n"                     // before any action: ignored, as the runtime ignores it
		"action: IA_Jump\n"
		"type: Button\n"
		"bind: Key Space\n"
		"action: Vehicles/IA_Steer\n"      // a subfolder of Input
		"type: axis\n"
		"action: IA_Custom.ext\n"          // the name has an extension: none is added
		"action: IA_Jump\n"                // the same action twice: one tag, one edge
		"action: ../../Outside\n"          // leaves the mount: a warning, no edge
		"action:\n");                      // no name: a warning
	const SAnalyzeResult result = tree.Analyze("Controls/Gameplay/IAM_Пілот.frtinputmap");
	ASSERT_EQ(result.Status, EImportStatus::Analyzed) << DescribeDiagnostics(result).CStr();
	EXPECT_EQ(GetNameTags(result, "Actions"), (std::vector<FString> { "IA_Jump", "Vehicles/IA_Steer", "IA_Custom.ext", "../../Outside" }));
	EXPECT_EQ(result.PrimaryName, "IAM_Пілот");

	// The map's mount's Input folder, whatever folder the map lives in.
	ASSERT_EQ(result.Edges.size(), 3u);
	EXPECT_NE(FindEdgeTo(result, "/Project/Input/IA_Jump.frtinputaction"), nullptr);
	EXPECT_NE(FindEdgeTo(result, "/Project/Input/Vehicles/IA_Steer.frtinputaction"), nullptr);
	EXPECT_NE(FindEdgeTo(result, "/Project/Input/IA_Custom.ext"), nullptr);

	ASSERT_EQ(CountDiagnostics(result, ESeverity::Warning, 0u), 2u) << DescribeDiagnostics(result).CStr();
	const SDiagnostic* outside = FindDiagnostic(result, "../../Outside");
	ASSERT_NE(outside, nullptr);
	EXPECT_EQ(outside->Line, 10u);
	EXPECT_EQ(outside->Column, 9u);
	EXPECT_EQ(outside->FieldPath, "actions.4.action");
	EXPECT_EQ(outside->File, "Content/Controls/Gameplay/IAM_Пілот.frtinputmap");
}

TEST(LegacyInputImporterTest, ActionTypeFollowsTheRuntimesParse)
{
	const CImporterTree tree("ActionTypes");
	tree.Write("Input/IA_Axis.frtinputaction", "version: 1\naction: IA_Axis\ntype: A_x-I s\n");
	tree.Write("Input/IA_Default.frtinputaction", "version: 1\naction: IA_Default\n");
	// An unparsable type keeps the kind an earlier line set; the runtime does not strip a trailing comment.
	tree.Write("Input/IA_Kept.frtinputaction", "action: IA_Kept\ntype: Axis\ntype: Trigger\ntype: Button # no\n");

	EXPECT_EQ(GetNameTags(tree.Analyze("Input/IA_Axis.frtinputaction"), "ValueType"), (std::vector<FString> { "Axis" }));
	EXPECT_EQ(GetNameTags(tree.Analyze("Input/IA_Default.frtinputaction"), "ValueType"), (std::vector<FString> { "Button" }));
	const SAnalyzeResult kept = tree.Analyze("Input/IA_Kept.frtinputaction");
	ASSERT_EQ(kept.Status, EImportStatus::Analyzed);
	EXPECT_EQ(GetNameTags(kept, "ValueType"), (std::vector<FString> { "Axis" }));
	ASSERT_EQ(CountDiagnostics(kept, ESeverity::Warning, 0u), 2u) << DescribeDiagnostics(kept).CStr();
	EXPECT_EQ(kept.Diagnostics[0].Line, 3u);
	EXPECT_EQ(kept.Diagnostics[1].Line, 4u);
	EXPECT_EQ(kept.ContentHash, HashFileStable(tree.File("Input/IA_Kept.frtinputaction")));
}
