// What Analyze costs per file kind (E3, design §5.11's cold-scan budget is F3's: 10k files incl. Analyze in 3 s on 16
// threads, so about 4.8 ms of one thread per file at most). Measured on one thread over copies of the Demo template's
// files with the OS cache warm (a cold cache adds the disk's latency, which F3 measures after a reboot); "cold" means
// nothing of the registry is reused - every file is read and parsed again. Each kind is the best of three passes
// (the machine is shared), judged as ../BenchmarkBudget.h says: the 4.8 ms per file that §5.11's cold scan leaves
// under FRT_PERF_STRICT=1, twice that as the regression guard of a whole Core-Test run (Sponza measured 3.7-8.3 ms in
// single passes there, 1.9 ms alone).
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <system_error>
#include <vector>

#include "../BenchmarkBudget.h"
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

// §5.11's cold scan per file of one thread (see above); the guard is twice it in either optimized build.
constexpr double CeilingMicroseconds = 4800.0;
constexpr bench::SBudget PerFileBudget { CeilingMicroseconds, -1.0, 2.0 * CeilingMicroseconds, 2.0 * CeilingMicroseconds };
constexpr uint32 Passes = FRT_BUILD_DEBUG ? 1u : 3u;   // Debug prints only
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
		double best = 1e30;
		for (uint32 pass = 0u; pass < Passes; ++pass)
		{
			uint32 analyzed = 0u;
			const auto start = std::chrono::steady_clock::now();
			for (const SAnalyzeInput& input : inputs)
			{
				analyzed += AnalyzeFile(input, settings, result) ? 1u : 0u;
			}
			best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kind.Copies);
			EXPECT_EQ(analyzed, kind.Copies) << kind.Name.GetData() << DescribeDiagnostics(result).CStr();
		}
		const FString name = FString::Format("AnalyzeUs.K{}", k);
		const FString note = FString::Format("({}, over {} files, best of {} passes)", FStringView(kind.Name), kind.Copies, Passes);
		bench::CheckBudget("[importers]", name.CStr(), best, PerFileBudget, "us/file", note.CStr());
	}
}
