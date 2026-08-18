// ImGui panel implementations for GameInstance.
// Split out of GameInstance.cpp — long-term these move out of GameInstance entirely.
// Master entry is DrawUI(), called once per frame from GameInstance::Tick.

#include "GameInstance.h"

#ifndef FRT_HEADLESS

#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <system_error>
#include <vector>

#include "imgui.h"

#include "EnginePaths.h"
#include "Window.h"
#include "Graphics/Camera.h"
#include "Graphics/Render/GraphicsCoreTypes.h"
#include "Graphics/Render/Renderer.h"
#include "Profiler/ProfilingExport.h"
#include "Profiler/SceneDescriptor.h"

NAMESPACE_FRT_START
using namespace graphics;

void GameInstance::DrawUI ()
{
	DrawStatsPanel();
	DrawRaytracingPanel();
	DrawSkyPanel();
	DrawEditorPanel();
	DrawDisplaySettingsPanel();
}

void GameInstance::DrawStatsPanel ()
{
	ImGui::Begin("Stats", nullptr);

	if (ProfilingSession.IsActive())
	{
		ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1),
			"PROFILING %u/%u  [%s]",
			ProfilingSession.ProfileIndex() + 1u, ProfilingSession.ProfileCount(),
			ProfilingSession.PhaseName());
		if (ProfilingSession.WarmupRemaining() > 0u)
		{
			ImGui::Text("  warmup  %u frames left", ProfilingSession.WarmupRemaining());
		}
		else
		{
			ImGui::Text("  measure %u / %u frames", Metrics.SampleCount(), Metrics.GetWindow());
		}
		// Per-config files already flushed as each profile completes — Stop just aborts.
		if (ImGui::Button("Stop profiling"))
		{
			ProfilingSession.Stop();
			RtSettings = SavedRtSettings;
			World.bAccumulationDirty = true;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("-> %s", SessionNameBuf);
		ImGui::Separator();
	}
	else
	{
		ImGui::SetNextItemWidth(160.0f);
		ImGui::InputText("##sessionName", SessionNameBuf, sizeof(SessionNameBuf));
		ImGui::SameLine();
		const bool bStart = ImGui::Button("Start profiling session");

		const std::filesystem::path dir = paths::GetProfilingDir() / SessionNameBuf;
		std::error_code ec;
		const bool bExists = std::filesystem::exists(dir, ec);
		if (bExists)
		{
			ImGui::TextColored(ImVec4(1, 0.5f, 0.2f, 1),
				"folder '%s' exists - Start will DELETE it", SessionNameBuf);
		}

		if (bStart && SessionNameBuf[0] != '\0')
		{
			SavedRtSettings = RtSettings;
			const auto [sw, sh] = Window->GetWindowSize();
			SessionSceneDesc = profiler::CaptureSceneDescriptor(
				World, *Camera, SkySettings, static_cast<uint32>(sw), static_cast<uint32>(sh));

			SessionDir = dir;
			std::filesystem::remove_all(SessionDir, ec);
			std::filesystem::create_directories(SessionDir, ec);

			ProfilingSession.Start();
			World.bAccumulationDirty = true;
		}
		ImGui::Separator();
	}

	ImGui::Text("FPS: %.2f", StatFps);
	ImGui::Text("MS/frame: %.2f", StatMsPerFrame);

	ImGui::Separator();
	ImGui::Text("GPU passes:");
	const auto& gpuResults = Renderer->GetGpuProfiler().GetLastResults();
	double gpuTotalMs = 0.0;
	for (uint32 i = 0; i < gpuResults.Count(); ++i)
	{
		const auto& r = gpuResults[i];
		ImGui::Text("  %-22s %7.3f ms", r.Name ? r.Name : "?", r.DurationMs);
		gpuTotalMs += r.DurationMs;
	}
	if (!gpuResults.IsEmpty())
	{
		ImGui::Text("  %-22s %7.3f ms", "(sum)", gpuTotalMs);
	}

	ImGui::Separator();
	const profiler::SFrameMetrics& cur = Metrics.Latest();
	const profiler::SFrameMetrics& avg = Metrics.Average();
	ImGui::Text("Ray counters    %-14s %-14s", "instant", "avg(N)");
	auto row = [] (const char* label, uint64 a, uint64 b)
	{
		ImGui::Text("  %-13s %-14llu %-14llu", label, a, b);
	};
	row("Primary",   cur.Counters.Values[profiler::RC_Primary],            avg.Counters.Values[profiler::RC_Primary]);
	row("Bounce",    cur.Counters.Values[profiler::RC_Bounce],             avg.Counters.Values[profiler::RC_Bounce]);
	row("SunShadow", cur.Counters.Values[profiler::RC_SunShadow],          avg.Counters.Values[profiler::RC_SunShadow]);
	row("SkyAtt",    cur.Counters.Values[profiler::RC_SkyShadowAttempted], avg.Counters.Values[profiler::RC_SkyShadowAttempted]);
	row("PortalOK",  cur.Counters.Values[profiler::RC_SkyShadowPortalPass],avg.Counters.Values[profiler::RC_SkyShadowPortalPass]);
	row("PortalRej", cur.Counters.Values[profiler::RC_SkyShadowPortalRej], avg.Counters.Values[profiler::RC_SkyShadowPortalRej]);
	row("ReachedSky",cur.Counters.Values[profiler::RC_SkyShadowReachedSky],avg.Counters.Values[profiler::RC_SkyShadowReachedSky]);
	row("Blocked",   cur.Counters.Values[profiler::RC_SkyShadowBlocked],   avg.Counters.Values[profiler::RC_SkyShadowBlocked]);
	row("PortalTests",cur.Counters.Values[profiler::RC_PortalTests],       avg.Counters.Values[profiler::RC_PortalTests]);

	ImGui::Separator();
	ImGui::Text("%% filtered      %6.1f%%        %6.1f%%", cur.PctFiltered * 100.0,   avg.PctFiltered * 100.0);
	ImGui::Text("sky efficiency  %6.1f%%        %6.1f%%", cur.SkyEfficiency * 100.0, avg.SkyEfficiency * 100.0);
	ImGui::Text("mean E[K]       %6.2f         %6.2f",    cur.MeanPortalTests,       avg.MeanPortalTests);
	ImGui::Text("rays / pixel    %6.2f         %6.2f",    cur.RaysPerPixel,          avg.RaysPerPixel);
	ImGui::Text("RT_Dispatch ms  %6.3f         %6.3f",    cur.RtDispatchMs,          avg.RtDispatchMs);
	ImGui::Text("avg window      %u / %u frames", Metrics.SampleCount(), Metrics.GetWindow());

	ImGui::Separator();
	{
		const auto [pw, ph] = Window->GetWindowSize();
		const profiler::SSceneDescriptor desc = profiler::CaptureSceneDescriptor(
			World, *Camera, SkySettings, static_cast<uint32>(pw), static_cast<uint32>(ph));
		ImGui::Text("Portals: %u   total screen: %.2f%%",
			desc.PortalCount, desc.PortalCoverageTotal * 100.0);
		for (uint32 i = 0; i < desc.Portals.Count(); ++i)
		{
			const profiler::SPortalInfo& p = desc.Portals[i];
			ImGui::Text("  #%u %-7s area %.2f m^2  screen %.3f%%",
				i, p.bEllipse ? "ellipse" : "rect", p.WorldArea, p.ScreenCoverage * 100.0);
			ImGui::Text("     dbg w0=%.3f ndc0=(%.2f,%.2f) clipN=%d ndcN=%d",
				p.DbgClipW0, p.DbgNdc0X, p.DbgNdc0Y, p.DbgClipCount, p.DbgNdcCount);
		}
	}
	ImGui::End();
}

void GameInstance::DrawRaytracingPanel ()
{
	// Live RT knobs — used by sweep studies (samples × bounces × RR) without shader recompile.
	SRtSettings& rt = RtSettings;
	ImGui::Begin("Raytracing", nullptr);

	// Parameters are driven by the session while it runs — lock the UI so manual edits
	// don't fight the sweep.
	const bool bLocked = ProfilingSession.IsActive();
	ImGui::BeginDisabled(bLocked);

	int samples = static_cast<int>(rt.SampleCount);
	int bounces = static_cast<int>(rt.MaxBounces);
	int rr = static_cast<int>(rt.RussianRouletteDepth);
	if (ImGui::SliderInt("Samples / pixel", &samples, 1, 64)) rt.SampleCount = static_cast<uint32>(samples);
	if (ImGui::SliderInt("Max bounces", &bounces, 0, 16)) rt.MaxBounces = static_cast<uint32>(bounces);
	if (ImGui::SliderInt("RR start depth", &rr, 0, 16)) rt.RussianRouletteDepth = static_cast<uint32>(rr);
	ImGui::Checkbox("Portal pre-filter", &rt.bPortalPreFilter);
	ImGui::Checkbox("Show portal meshes", &rt.bShowPortalMeshes);
	ImGui::Checkbox("Collect ray counters", &rt.bCollectCounters);

	if (ImGui::Button("Reset accumulation"))
	{
		World.bAccumulationDirty = true;
	}

	ImGui::EndDisabled();
	ImGui::End();
}

void GameInstance::DrawSkyPanel ()
{
	// TimeOfDay drives SkySettings via ComputeSkyFromTimeOfDay when the toggle is on;
	// otherwise the individual fields are tweakable directly.
	graphics::SSkyConstants& sky = SkySettings;
	float& tod = TimeOfDay;
	bool& useTod = bUseTimeOfDay;

	ImGui::Begin("Sky / Sun");
	ImGui::Checkbox("Drive from Time of Day", &useTod);
	if (useTod)
	{
		ImGui::SliderFloat("Time of Day", &tod, 0.0f, 1.0f, "%.3f");
		graphics::ComputeSkyFromTimeOfDay(tod, sky);
	}
	else
	{
		ImGui::DragFloat3("Sun direction", &sky.SunDirection.x, 0.01f, -1.0f, 1.0f);
		ImGui::ColorEdit3("Sun color", &sky.SunColor.R);
		ImGui::SliderFloat("Sun intensity", &sky.SunIntensity, 0.0f, 50.0f);
		ImGui::ColorEdit3("Sky zenith", &sky.SkyZenithColor.R);
		ImGui::ColorEdit3("Sky horizon", &sky.SkyHorizonColor.R);
		ImGui::ColorEdit3("Ground", &sky.GroundColor.R);
		ImGui::SliderFloat("Sky intensity", &sky.SkyIntensity, 0.0f, 5.0f);
		ImGui::SliderFloat("Horizon softness", &sky.HorizonSoftness, 0.0f, 1.0f);
	}
	ImGui::End();
}

void GameInstance::DrawEditorPanel ()
{
	// Camera transform + entity transform editor. Modifying any transform marks scene
	// topology dirty so AS rebuilds and accumulation resets.
	ImGui::Begin("Editor");

	// ----- Camera -----
	if (Camera)
	{
		ImGui::SeparatorText("Camera");
		Vector3f camPos = Camera->Transform.GetTranslation();
		Vector3f camRot = Camera->Transform.GetRotation();
		float camMovementSpeed = Camera->MovementSpeed;
		float camRotationSpeed = Camera->RotationSpeed;
		bool camChanged = false;
		camChanged |= ImGui::DragFloat3("position", &camPos.x, 0.05f);
		camChanged |= ImGui::DragFloat3("rotation", &camRot.x, 0.01f);
		ImGui::SliderFloat("move speed", &camMovementSpeed, 0.001f, 50.0f);
		ImGui::SliderFloat("rot  speed", &camRotationSpeed, 0.001f, 5.0f);

		Camera->MovementSpeed = camMovementSpeed;
		Camera->RotationSpeed = camRotationSpeed;
		if (camChanged)
		{
			Camera->Transform.SetTranslation(camPos);
			Camera->Transform.SetRotation(camRot);
			World.bAccumulationDirty = true;
		}
		else if (ImGui::Button("Reset Camera"))
		{
			Camera->Transform = CameraInitialTransform;
			World.bAccumulationDirty = true;
		}
	}

	// ----- Entity selection + transform -----
	ImGui::SeparatorText("Entity");
	auto& entities = World.GetEntities();
	const int32 entityCount = static_cast<int32>(entities.Count());

	if (SelectedEntityIndex >= entityCount)
	{
		SelectedEntityIndex = -1;
	}

	const char* preview = "(none)";
	if (SelectedEntityIndex >= 0 && SelectedEntityIndex < entityCount)
	{
		CEntity* sel = entities[SelectedEntityIndex].GetRawIgnoringLifetime();
		preview = (sel && !sel->Name.empty()) ? sel->Name.c_str() : "(unnamed)";
	}
	if (ImGui::BeginCombo("Selected", preview))
	{
		if (ImGui::Selectable("(none)", SelectedEntityIndex == -1))
		{
			SelectedEntityIndex = -1;
		}
		for (int32 i = 0; i < entityCount; ++i)
		{
			CEntity* e = entities[i].GetRawIgnoringLifetime();
			const char* label = (e && !e->Name.empty()) ? e->Name.c_str() : "(unnamed)";
			const bool selected = (SelectedEntityIndex == i);
			ImGui::PushID(i);
			if (ImGui::Selectable(label, selected))
			{
				SelectedEntityIndex = i;
			}
			if (selected) ImGui::SetItemDefaultFocus();
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}

	if (SelectedEntityIndex >= 0 && SelectedEntityIndex < entityCount)
	{
		CEntity* ent = entities[SelectedEntityIndex].GetRawIgnoringLifetime();
		if (ent)
		{
			char nameBuf[64];
			const size_t nameLen = ent->Name.size() < sizeof(nameBuf) - 1 ? ent->Name.size() : sizeof(nameBuf) - 1;
			std::memcpy(nameBuf, ent->Name.data(), nameLen);
			nameBuf[nameLen] = '\0';
			if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
			{
				ent->Name = nameBuf;
			}

			Vector3f pos = ent->Transform.GetTranslation();
			Vector3f rot = ent->Transform.GetRotation();
			Vector3f scl = ent->Transform.GetScale();
			bool changed = false;
			if (ImGui::DragFloat3("Position", &pos.x, 0.05f)) changed = true;
			if (ImGui::DragFloat3("Rotation", &rot.x, 0.01f)) changed = true;
			if (ImGui::DragFloat3("Scale", &scl.x, 0.01f, 0.001f, 1000.0f)) changed = true;
			if (changed)
			{
				ent->Transform.SetTranslation(pos);
				ent->Transform.SetRotation(rot);
				ent->Transform.SetScale(scl);
				World.bSceneTopologyDirty = true;
				World.bAccumulationDirty = true;
			}
		}
	}
	ImGui::End();
}

void GameInstance::DrawDisplaySettingsPanel ()
{
	// TODO: revisit to remove per-frame reallocations.
	ImGui::Begin("DisplaySettings");

	const auto strToChar = [] (void* UserData, int Idx) -> const char*
	{
		return static_cast<std::string*>(UserData)[Idx].c_str();
	};

	SDisplaySettings& displaySettings = UserSettings.DisplaySettings;

	{
		const auto monitorNames = DisplayOptions.GetNames();
		auto labelMonitor = "Monitor";
		ImGui::Combo(
			labelMonitor, &displaySettings.MonitorIndex, strToChar, (void*)monitorNames.data(),
			(int)monitorNames.size());
	}

	std::span<const SResolution> resolutions = DisplayOptions.GetResolutions(displaySettings.MonitorIndex);
	displaySettings.ResolutionIndex = math::ClampIndex(displaySettings.ResolutionIndex, resolutions.size());

	{
		// TODO: strs should be pre-made or at least pre-allocated
		std::vector<std::string> resolutionStrs;
		resolutionStrs.reserve(resolutions.size());
		for (const auto& res : resolutions)
		{
			resolutionStrs.emplace_back(res.ToString(':'));
		}

		auto labelResolution = "Resolution";
		ImGui::BeginDisabled(displaySettings.IsFullscreen());
		ImGui::Combo(
			labelResolution, &displaySettings.ResolutionIndex, strToChar, resolutionStrs.data(),
			(int)resolutionStrs.size());
		ImGui::EndDisabled();
	}

	std::span<const uint64> refreshRates = DisplayOptions.GetRefreshRates(
		displaySettings.MonitorIndex, displaySettings.ResolutionIndex);
	{
		displaySettings.RefreshRateIndex = math::ClampIndex(displaySettings.RefreshRateIndex, refreshRates.size());

		std::vector<std::string> rRStrs;
		rRStrs.reserve(refreshRates.size());
		for (const auto& rr : refreshRates)
		{
			uint32 numerator, denominator;
			math::DecodeTwoFromOne(rr, numerator, denominator);
			rRStrs.emplace_back(std::format("{:.2f}", (float)numerator / (float)denominator));
		}

		auto labelRR = "RefreshRate";
		ImGui::BeginDisabled(!displaySettings.IsFullscreen());
		ImGui::Combo(labelRR, &displaySettings.RefreshRateIndex, strToChar, rRStrs.data(), (int)rRStrs.size());
		ImGui::EndDisabled();
	}

	{
		const auto modeNames = enum_::GetValueNames<EFullscreenMode>();
		auto labelFullscreen = "Fullscreen";
		ImGui::SliderInt(
			labelFullscreen,
			(int*)&displaySettings.FullscreenMode,
			1, (int32)EFullscreenMode::Borderless,
			modeNames[(int32)displaySettings.FullscreenMode].data(),
			ImGuiSliderFlags_AlwaysClamp);
	}

	{
		const auto renderModeNames = enum_::GetValueNames<ERenderMode>();
		auto labelRenderMode = "RenderMode";
		ImGui::SliderInt(
			labelRenderMode,
			(int*)&displaySettings.RenderMode,
			0, (int32)ERenderMode::Count - 1,
			renderModeNames[(int32)displaySettings.RenderMode].data(),
			ImGuiSliderFlags_AlwaysClamp);
	}

	{
		auto labelVSync = "Enable VSync";
		ImGui::Checkbox(labelVSync, &displaySettings.bVSync);
	}

	if (ImGui::Button("Apply"))
	{
		if (displaySettings.IsFullscreen())
		{
			const SResolution fullscreenResolution = DisplayOptions.GetFullscreenResolution(
				displaySettings.MonitorIndex);
			const auto resIt = std::ranges::find(resolutions, fullscreenResolution);
			int32 resIndex = static_cast<int32>(std::distance(resolutions.begin(), resIt));
			resIndex = math::ClampIndex(resIndex, resolutions.size());
			displaySettings.ResolutionIndex = resIndex;
		}

		GetRenderer()->SetRenderMode(displaySettings.RenderMode);
		GetRenderer()->bVSyncEnabled = displaySettings.bVSync;

		uint32 numerator = 0;
		uint32 denominator = 0;
		math::DecodeTwoFromOne(refreshRates[displaySettings.RefreshRateIndex], numerator, denominator);
		GetRenderer()->DisplayRefreshRate = { numerator, denominator };

		Window->SetDisplaySettings(displaySettings, DisplayOptions);

		// Persist on Apply rather than on every widget edit: the panel writes straight into
		// displaySettings, so saving per-frame would rewrite the file while a slider is
		// being dragged. Applied and saved stay the same thing this way.
		SaveUserSettings(Config, UserSettings, DisplayOptions);
		Config.Save();
	}

	ImGui::End();
}

NAMESPACE_FRT_END

#endif // !FRT_HEADLESS
