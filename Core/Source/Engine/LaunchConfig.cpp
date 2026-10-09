#include "Engine/LaunchConfig.h"

#include <optional>

#include "WorldScene.h"
#include "Config/Config.h"
#include "Containers/String.h"

#if !defined(FRT_HEADLESS)
#include "Graphics/Render/Renderer.h"
#include "Graphics/Render/Passes/Denoiser.h"
#include "Graphics/Render/Scene/GpuScene.h"
#endif


namespace frt
{
bool ParseVector3 (FStringView Text, Vector3f& Out)
{
	float components[3] = {};
	uint32 count = 0u;
	for (const FStringView token : Text.Split(',', EStringSplit::Trim))
	{
		const std::optional<float> value = token.ToFloat();
		if (count == 3u || !value)
		{
			return false;
		}
		components[count++] = *value;
	}
	if (count != 3u)
	{
		return false;
	}
	Out = Vector3f(components[0], components[1], components[2]);
	return true;
}

#if !defined(FRT_HEADLESS)
void ApplyRenderLaunchConfig (const config::CConfig& Config, graphics::CRenderer& Renderer)
{
	using namespace graphics;
	SRenderSettings& render = Renderer.Profile.Frame;
	SPathTracerSettings& pt = Renderer.Profile.PathTracer;

	const FString mode = Config.Get("Render", "Mode", "RealTime");
	render.Mode = (mode == "Reference" || mode == "reference") ? EPathTracingMode::Reference : EPathTracingMode::RealTime;
	render.RenderScale = Config.Get("Render", "RenderScale", render.RenderScale);
	render.bDenoiser = Config.Get("Render", "Denoiser", render.bDenoiser);
	render.ReferenceSampleTarget = Config.Get("Render", "SampleTarget", render.ReferenceSampleTarget);
	render.bCollectRayCounters = Config.Get("Render", "CollectRayCounters", render.bCollectRayCounters);
	const FString upscaler = Config.Get("Render", "Upscaler", "Temporal");
	if (upscaler == "None" || upscaler == "Off")
	{
		Renderer.SetUpscalerKind(EUpscalerKind::None);
	}

	pt.SamplesPerPixel = Config.Get("Render", "Spp", pt.SamplesPerPixel);
	pt.MaxBounces = Config.Get("Render", "MaxBounces", pt.MaxBounces);
	pt.RussianRouletteDepth = Config.Get("Render", "RussianRouletteDepth", pt.RussianRouletteDepth);
	pt.RisCandidates = Config.Get("Render", "RisCandidates", pt.RisCandidates);
	pt.RisCandidatesSecondary = Config.Get("Render", "RisCandidatesSecondary", pt.RisCandidatesSecondary);
	pt.IndirectClamp = Config.Get("Render", "IndirectClamp", pt.IndirectClamp);
	pt.bShaderExecutionReordering = Config.Get("Render", "SER", pt.bShaderExecutionReordering);
	pt.bInlineRayQueries = Config.Get("Render", "InlineRT", pt.bInlineRayQueries);
	pt.bNormalMaps = Config.Get("Render", "NormalMaps", pt.bNormalMaps);
	pt.bPortalGuiding = Config.Get("Render", "PortalGuiding", pt.bPortalGuiding);
	pt.bPortalPreFilter = Config.Get("Render", "PortalPreFilter", pt.bPortalPreFilter);
	pt.bPrimarySurfaceReplacement = Config.Get("Render", "PSR", pt.bPrimarySurfaceReplacement);
	pt.bDisableNee = Config.Get("Render", "DisableNee", pt.bDisableNee);
	pt.bDisableBsdfMis = Config.Get("Render", "DisableBsdfMis", pt.bDisableBsdfMis);
	pt.bWhiteFurnace = Config.Get("Render", "WhiteFurnace", pt.bWhiteFurnace);
	pt.DebugMode = Config.Get("Render", "DebugMode", pt.DebugMode);

	graphics::SRadianceCacheSettings& rc = Renderer.Profile.RadianceCache;
	rc.bEnabled = Config.Get("Render", "RadianceCache", rc.bEnabled);
	rc.CapacityLog2 = Config.Get("Render", "RcCapacityLog2", rc.CapacityLog2);
	rc.CellScale = Config.Get("Render", "RcCellScale", rc.CellScale);
	rc.UpdateTileSize = Config.Get("Render", "RcUpdateTile", rc.UpdateTileSize);
	rc.AccumulationFrames = Config.Get("Render", "RcAccumFrames", rc.AccumulationFrames);
	rc.StaleFrames = Config.Get("Render", "RcStaleFrames", rc.StaleFrames);
	rc.RoughnessThreshold = Config.Get("Render", "RcRoughness", rc.RoughnessThreshold);
	rc.FootprintScale = Config.Get("Render", "RcFootprint", rc.FootprintScale);
	rc.MinSampleWeight = Config.Get("Render", "RcMinSamples", rc.MinSampleWeight);
	rc.SampleClamp = Config.Get("Render", "RcSampleClamp", rc.SampleClamp);
	rc.bFeedback = Config.Get("Render", "RcFeedback", rc.bFeedback);
	rc.bJitter = Config.Get("Render", "RcJitter", rc.bJitter);
	rc.FeedbackMaxDepth = Config.Get("Render", "RcFeedbackDepth", rc.FeedbackMaxDepth);
	rc.SpecularShareMax = Config.Get("Render", "RcSpecShare", rc.SpecularShareMax);
	Renderer.GetScene().bPortalWeightedLightSelection =
		Config.Get("Render", "PortalLightSelection", Renderer.GetScene().bPortalWeightedLightSelection);

	graphics::SPostProcessSettings& post = Renderer.Profile.Post;
	post.bAutoExposure = Config.Get("Render", "AutoExposure", post.bAutoExposure);
	post.ManualExposureEV = Config.Get("Render", "ExposureEV", post.ManualExposureEV);
	post.ExposureCompensationEV = Config.Get("Render", "ExposureCompensationEV", post.ExposureCompensationEV);
	post.bBloom = Config.Get("Render", "Bloom", post.bBloom);
	// [Post] keys (-Post:Key=Value): bloom and metering parameters, for A/B captures.
	post.BloomIntensity = Config.Get("Post", "BloomIntensity", post.BloomIntensity);
	post.BloomRadius = Config.Get("Post", "BloomRadius", post.BloomRadius);
	post.BloomFalloff = Config.Get("Post", "BloomFalloff", post.BloomFalloff);
	post.BloomMips = Config.Get("Post", "BloomMips", post.BloomMips);
	post.HistogramLowPercentile = Config.Get("Post", "LowPercentile", post.HistogramLowPercentile);
	post.HistogramHighPercentile = Config.Get("Post", "HighPercentile", post.HistogramHighPercentile);
	post.MeteringCenterWeight = Config.Get("Post", "CenterWeight", post.MeteringCenterWeight);
	post.ToneMapper = static_cast<graphics::EToneMapper>(Config.Get("Post", "ToneMapper", static_cast<uint32>(post.ToneMapper)));

	// [Taa] keys (-Taa:Key=Value): in-house TAA / TAAU settings, for A/B captures.
	{
		graphics::STemporalUpscalerSettings& t = Renderer.Profile.Taa;
		t.HistoryBlend = Config.Get("Taa", "HistoryBlend", t.HistoryBlend);
		t.Sharpness = Config.Get("Taa", "Sharpness", t.Sharpness);
		t.VarianceGamma = Config.Get("Taa", "VarianceGamma", t.VarianceGamma);
		t.TemporalGamma = Config.Get("Taa", "TemporalGamma", t.TemporalGamma);
		t.bAntiFlicker = Config.Get("Taa", "AntiFlicker", t.bAntiFlicker);
		t.bVarianceClip = Config.Get("Taa", "VarianceClip", t.bVarianceClip);
		t.bLanczosKernel = Config.Get("Taa", "Lanczos", t.bLanczosKernel);
		t.bCatmullRomHistory = Config.Get("Taa", "CatmullRom", t.bCatmullRomHistory);
		t.bDepthTest = Config.Get("Taa", "DepthTest", t.bDepthTest);
		t.DebugMode = Config.Get("Taa", "Debug", t.DebugMode);
	}

	// [Denoiser] keys (-Denoiser:Key=Value), parsed by the pass itself.
	graphics::CDenoiser::ApplyConfig(Config, Renderer.Profile.Denoiser);
	if (Config.Has("Display", "VSync"))
	{
		Renderer.bVSyncEnabled = Config.Get("Display", "VSync", Renderer.bVSyncEnabled);
	}
}

void ApplyViewLaunchConfig (const config::CConfig& Config, graphics::CRenderView& View)
{
	// Session-only override of the persisted [Settings.Display] RenderMode (not written back).
	const FString renderMode = Config.Get("Render", "RenderMode", "");
	if (renderMode == "Raytracing" || renderMode == "raytracing")
	{
		View.SetRenderMode(graphics::ERenderMode::Raytracing);
	}
	else if (renderMode == "Raster" || renderMode == "raster")
	{
		View.SetRenderMode(graphics::ERenderMode::Raster);
	}
}
#endif

void ApplySceneLaunchConfig (const config::CConfig& Config, CWorldScene& World)
{
	World.Environment.TimeOfDay = Config.Get("Scene", "TimeOfDay", World.Environment.TimeOfDay);
	World.Camera.FieldOfViewY = Config.Get("Render", "FieldOfViewY", World.Camera.FieldOfViewY);
	Vector3f camPos;
	if (ParseVector3(Config.Get("Scene", "CameraPosition", ""), camPos))
	{
		World.Camera.Transform.SetTranslation(camPos);
	}
	Vector3f camRot;
	if (ParseVector3(Config.Get("Scene", "CameraRotation", ""), camRot))
	{
		World.Camera.Transform.SetRotation(camRot);
	}
}
}
