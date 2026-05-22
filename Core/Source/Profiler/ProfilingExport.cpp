#include "Profiler/ProfilingExport.h"

#include <cstdio>
#include <fstream>
#include <string>


namespace frt::profiler
{
bool WriteConfigTxt (
	const std::filesystem::path& Dir,
	uint32 Index,
	const SProfileConfig& Config,
	const SSceneDescriptor& Scene)
{
	const std::filesystem::path path = Dir / (std::to_string(Index) + ".txt");
	std::ofstream f(path);
	if (!f)
	{
		std::printf("[profiler] cannot open %s\n", path.string().c_str());
		std::fflush(stdout);
		return false;
	}

	// Test settings (the configuration under test).
	f << "config_index: "       << Index << "\n";
	f << "spp: "                << Config.SampleCount << "\n";
	f << "max_bounces: "        << Config.MaxBounces << "\n";
	f << "rr_depth: "           << Config.RussianRouletteDepth << "\n";
	f << "portal_prefilter: "   << (Config.bPortalPreFilter ? 1 : 0) << "\n";

	// Static scene data.
	f << "render_width: "  << Scene.RenderWidth  << "\n";
	f << "render_height: " << Scene.RenderHeight << "\n";
	f << "entity_count: "  << Scene.EntityCount  << "\n";
	f << "triangle_count: " << Scene.TriangleCount << "\n";
	f << "point_lights: "       << Scene.PointLightCount       << "\n";
	f << "directional_lights: " << Scene.DirectionalLightCount << "\n";
	f << "areaquad_lights: "    << Scene.AreaQuadLightCount    << "\n";
	f << "portal_count: " << Scene.PortalCount << "\n";
	f << "portal_coverage_total: " << Scene.PortalCoverageTotal << "\n";
	f << "camera_position: "
	  << Scene.CameraPosition.x << " " << Scene.CameraPosition.y << " " << Scene.CameraPosition.z << "\n";
	f << "camera_rotation: "
	  << Scene.CameraRotation.x << " " << Scene.CameraRotation.y << " " << Scene.CameraRotation.z << "\n";
	f << "sun_direction: "
	  << Scene.Sky.SunDirection.x << " " << Scene.Sky.SunDirection.y << " " << Scene.Sky.SunDirection.z << "\n";
	f << "sun_intensity: " << Scene.Sky.SunIntensity << "\n";
	f << "sky_intensity: " << Scene.Sky.SkyIntensity << "\n";
	for (uint32 i = 0; i < Scene.Portals.Count(); ++i)
	{
		const SPortalInfo& p = Scene.Portals[i];
		f << "portal[" << i << "].shape: "           << (p.bEllipse ? "ellipse" : "rect") << "\n";
		f << "portal[" << i << "].world_area: "      << p.WorldArea << "\n";
		f << "portal[" << i << "].screen_coverage: " << p.ScreenCoverage << "\n";
	}

	std::printf("[profiler] wrote %s\n", path.string().c_str());
	std::fflush(stdout);
	return true;
}


bool WriteConfigFramesCsv (
	const std::filesystem::path& Dir,
	uint32 Index,
	const SProfileResult& Result)
{
	const std::filesystem::path path = Dir / (std::to_string(Index) + ".csv");
	std::ofstream f(path);
	if (!f)
	{
		std::printf("[profiler] cannot open %s\n", path.string().c_str());
		std::fflush(stdout);
		return false;
	}

	f << "frame,"
	     "rt_ms,gpu_ms,frame_ms,"
	     "pct_filtered,sky_efficiency,rays_per_pixel,tlas_dispatches,mean_portal_tests,"
	     "primary,bounce,sun_shadow,sky_attempted,portal_pass,portal_rej,reached_sky,blocked,portal_tests\n";

	for (std::size_t i = 0; i < Result.Frames.size(); ++i)
	{
		const SFrameMetrics& m = Result.Frames[i];
		f << i << ','
		  << m.RtDispatchMs << ','
		  << m.GpuTotalMs << ','
		  << m.FrameMs << ','
		  << m.PctFiltered << ','
		  << m.SkyEfficiency << ','
		  << m.RaysPerPixel << ','
		  << m.TlasDispatches << ','
		  << m.MeanPortalTests << ','
		  << m.Counters.Values[RC_Primary] << ','
		  << m.Counters.Values[RC_Bounce] << ','
		  << m.Counters.Values[RC_SunShadow] << ','
		  << m.Counters.Values[RC_SkyShadowAttempted] << ','
		  << m.Counters.Values[RC_SkyShadowPortalPass] << ','
		  << m.Counters.Values[RC_SkyShadowPortalRej] << ','
		  << m.Counters.Values[RC_SkyShadowReachedSky] << ','
		  << m.Counters.Values[RC_SkyShadowBlocked] << ','
		  << m.Counters.Values[RC_PortalTests] << '\n';
	}

	std::printf("[profiler] wrote %s (%zu frames)\n", path.string().c_str(), Result.Frames.size());
	std::fflush(stdout);
	return true;
}
}
