#pragma once

#include "Graphics/Render/RenderContext.h"
#include "Graphics/Render/RenderSettings.h"

namespace frt::config
{
class CConfig;
}

namespace frt::graphics
{
/**
 * In-house real-time denoiser for the path tracer's demodulated signals (ReLAX / ReBLUR class).
 *
 * Inputs (render resolution, produced by PathTracer.hlsl, see Shared/PathTracerData.h):
 *   Diffuse          RGBA16F  rgb = diffuse radiance / diffuse albedo, a = hit distance (0 = none)
 *   Specular         RGBA16F  rgb = specular radiance / specular albedo, a = hit distance
 *   NormalRoughness  RGBA16F  xyz = world shading normal (unit), w = linear roughness
 *   ViewZ            R32F     linear view depth; >= 1e5 means sky (no surface)
 *   MotionVectors    RG16F    (previous - current) pixel position, render pixels
 * Outputs: denoised Diffuse / Specular in the same demodulated form (RGBA16F); the renderer
 * remodulates with the albedo buffers and adds emission.
 *
 * The input can be extremely sparse (demo scene without the radiance cache, 1 spp: 99 % of the
 * samples exactly zero, the energy in rare samples up to thousands of times the local mean), so
 * every statistic and weight is one that stays unbiased for such signals - neighbourhood boxes,
 * rank filters and value-dependent normalised weights are not; the shader headers give the
 * measurements behind each choice. Pipeline per frame (each part toggleable through Settings):
 *   1. Temporal (Temporal.hlsl): reprojection with motion vectors (diffuse) and hit-distance virtual
 *      motion for low-roughness specular, per-tap disocclusion test (plane distance + normal),
 *      history length, luminance moments, a per-pixel fast history (anti-lag reference), an input
 *      firefly limit far above last frame's denoised value, per-8x8-tile anti-lag sums and the
 *      noise texture (deviation of the accumulated mean, specular filter extent, hit distance).
 *   2. History fix (HistoryFix.hlsl): young pixels only (disocclusions) get a wide geometry-weighted
 *      fill from their converged neighbours.
 *   3. A-trous (Atrous.hlsl, 5 iterations): energy-conserving edge-stopping filter (symmetric pair
 *      weights: plane, normal, luminance deviation; specular also roughness, lobe, hit distance and
 *      a noise-driven extent). Iteration 0 writes the history (optionally its filtered output,
 *      recurrent) and applies the anti-lag significance test pooled over 5x5 tiles.
 */

struct SDenoiserInputs
{
	rhi::SGpuTexture* Diffuse = nullptr;
	rhi::SGpuTexture* Specular = nullptr;
	rhi::SGpuTexture* NormalRoughness = nullptr;
	rhi::SGpuTexture* ViewZ = nullptr;
	rhi::SGpuTexture* MotionVectors = nullptr;
	bool bResetHistory = false;          // camera cut / settings change
};

class FRT_CORE_API CDenoiser
{
public:
	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines);
	void Shutdown (rhi::CDevice& Device);
	void Resize (rhi::CDevice& Device, uint32 Width, uint32 Height);

	// Records all passes. Leaves the input textures in NON_PIXEL_SHADER_RESOURCE.
	void Execute (CRenderContext& Context, const SDenoiserInputs& Inputs);

	// Denoised outputs (RGBA16F, rgb = demodulated radiance, a = denoised hit distance), left in
	// NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE. Sky pixels hold the untouched input.
	rhi::SGpuTexture& GetDiffuseOutput ();
	rhi::SGpuTexture& GetSpecularOutput ();

	// Debug: per-pixel history length (R16F, length / MaxHistoryLength in [0, 1], signal chosen by
	// Settings.HistoryDebugSignal; written only with Settings.bHistoryLengthDebug), may return nullptr.
	rhi::SGpuTexture* GetHistoryLengthTexture ();

	void DrawSettingsUi (SDenoiserSettings& Edit);

	// Session overrides from the [Denoiser] config section (launch arguments -Denoiser:Key=Value,
	// keys = SDenoiserSettings member names without the b prefix, e.g. -Denoiser:AntiLag=false).
	static void ApplyConfig (const config::CConfig& Config, SDenoiserSettings& Out);

	SDenoiserSettings Settings;

private:
	struct SImpl;
	SImpl* Impl = nullptr;
};
}
