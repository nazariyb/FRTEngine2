// What Analyze costs per file kind (E3, design §5.11's cold-scan budget is F3's: 10k files incl. Analyze in 3 s on 16
// threads, so about 4.8 ms of one thread per file at most). Measured on one thread over copies of the Demo template's
// files with the OS cache warm (a cold cache adds the disk's latency, which F3 measures after a reboot); "cold" means
// nothing of the registry is reused - every file is read and parsed again. Prints microseconds per file; checks only
// that every file analyzed, and a generous ceiling outside Debug.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <system_error>
#include <vector>

#include "ImporterTestUtils.h"

#include "BuildConfig.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::test::importers;


namespace
{
struct SKind
{
	FStringLiteral Name;
	FStringLiteral Source;      // below Templates/Demo/Content
	FStringLiteral Extension;
	uint32 Copies;
};

constexpr SKind Kinds[] = {
	{ "Texture .png (512x512 RGBA)", "Models/Cube/Cube_BaseColor.png", ".png", 200u },
	{ "Texture .jpg (1024x1024)", "Models/Sponza/8773302468495022225.jpg", ".jpg", 200u },
	{ "Model.Gltf (Duck)", "Models/Duck/Duck.gltf", ".gltf", 100u },
	{ "Model.Gltf (Sponza, 95 objects)", "Models/Sponza/Sponza.gltf", ".gltf", 20u },
	{ "Material.Legacy v2", "Models/Duck/Duck_mat0.frtmat.yml", ".frtmat.yml", 200u },
	{ "Input.Legacy map", "Input/IAM_Editor.frtinputmap", ".frtinputmap", 200u },
	{ "Input.Legacy action", "Input/IA_MoveForward.frtinputaction", ".frtinputaction", 200u },
};

// Generous: Analyze reads headers or small text files; the budget above leaves ~4.8 ms per file.
constexpr double CeilingMicroseconds = 4800.0;
}


TEST(AnalyzeBenchmarkTest, ColdAnalyzeCostPerFileKind)
{
	const CImporterTree tree("Benchmark");
	const std::filesystem::path demo = paths::GetEngineDir() / "Templates" / "Demo" / "Content";
	std::error_code ec;
	const CMountTable mounts = [&]
	{
		for (uint32 k = 0u; k < std::size(Kinds); ++k)
		{
			const std::filesystem::path folder = tree.File(FString::Format("K{}", k));
			std::filesystem::create_directories(folder, ec);
			for (uint32 i = 0u; i < Kinds[k].Copies; ++i)
			{
				std::filesystem::copy_file(demo / Kinds[k].Source.ToPath(), folder / FString::Format("f{}{}", i, Kinds[k].Extension).ToPath(),
					std::filesystem::copy_options::overwrite_existing, ec);
			}
		}
		return tree.Resolve();
	}();
	const SMount& project = *mounts.Find("Project");

	const bool bOptimized = !FRT_BUILD_DEBUG;
	for (uint32 k = 0u; k < std::size(Kinds); ++k)
	{
		const SKind& kind = Kinds[k];
		const SEffectiveFileSettings settings = MakeSettings(FindImporterForFile(FString::Format("x{}", kind.Extension)));
		std::vector<SAnalyzeInput> inputs;
		for (uint32 i = 0u; i < kind.Copies; ++i)
		{
			inputs.push_back(*SAnalyzeInput::FromMount(project, CImporterTree::MountPath(FString::Format("K{}/f{}{}", k, i, kind.Extension))));
		}

		SAnalyzeResult result;
		uint32 analyzed = 0u;
		const auto start = std::chrono::steady_clock::now();
		for (const SAnalyzeInput& input : inputs)
		{
			analyzed += AnalyzeFile(input, settings, result) ? 1u : 0u;
		}
		const double microseconds = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kind.Copies;

		std::printf("[importers] %-34s %9.1f us/file over %u files%s\n", kind.Name.GetData(), microseconds, kind.Copies,
			bOptimized ? "" : " (Debug: not judged)");
		EXPECT_EQ(analyzed, kind.Copies) << kind.Name.GetData() << DescribeDiagnostics(result).CStr();
		if (bOptimized)
		{
			EXPECT_LT(microseconds, CeilingMicroseconds) << kind.Name.GetData();
		}
	}
}
