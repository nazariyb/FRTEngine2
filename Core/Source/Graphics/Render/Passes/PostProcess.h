#pragma once

#include "Graphics/Render/RenderContext.h"
#include "Graphics/Render/RenderSettings.h"

namespace frt::graphics
{

/**
 * Display pipeline after the HDR image is final (display resolution):
 *   1. Auto exposure: log2-luminance histogram of 4x4 block means (256 bins, centre-weighted), percentile-trimmed
 *      average, temporal adaptation with separate speeds for brightening / darkening, clamped
 *      to [MinEV, MaxEV], plus compensation. Exposure lives in a tiny GPU buffer (no readback
 *      stall); the CPU sees it two frames late for the UI.
 *   2. Bloom: energy-conserving dual-filter pyramid (13-tap downsample with Karis average on the
 *      first level against fireflies, 3x3 tent upsample), levels weighted by a geometric energy
 *      falloff (point-spread function ~ r^-3), mixed linearly by Intensity.
 *   3. Tone mapping + grading (exposure, white balance, saturation, contrast), output transform
 *      to the swap chain encoding (sRGB OETF for SDR; scRGB / PQ hooks for HDR), and
 *      triangular-PDF blue-noise dithering before quantisation to 8 bits.
 * The final pass is a full-screen triangle into the back buffer RTV.
 *
 * Exposure convention: EV100 of a reflected-light meter, EV = log2(L) + 3 (ISO 100, K = 12.5);
 * exposure multiplier = 2^(Compensation - EV), so the metered average lands at 1/8. HDR inputs
 * are treated as scene radiance x g_Frame.PreExposure (divided out for metering and display).
 * Implementation: PostProcess.cpp, shaders in Shaders/PostProcess/, constants and the exposure
 * buffer layout in Shaders/Shared/PostProcessData.h. GPU scopes: PostProcess_Exposure,
 * PostProcess_Bloom, PostProcess_Final.
 */

struct SPostProcessInputs
{
	rhi::SGpuTexture* HdrColor = nullptr;        // display resolution, RGBA16F
	D3D12_CPU_DESCRIPTOR_HANDLE OutputRtv = {};  // back buffer
	DXGI_FORMAT OutputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	uint32 OutputWidth = 0u;
	uint32 OutputHeight = 0u;
	bool bResetExposure = false;                 // jump straight to the target exposure
	// Optional debug visualisation that replaces the image (already in display space or linear,
	// see DebugIsLinear and SPostProcessSettings::bDebugToneMap). Point-sampled at any size. Null = none.
	rhi::SGpuTexture* DebugTexture = nullptr;
	bool bDebugIsLinear = true;
};

class FRT_CORE_API CPostProcess
{
public:
	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines);
	void Shutdown (rhi::CDevice& Device);
	void Resize (rhi::CDevice& Device, uint32 DisplayWidth, uint32 DisplayHeight);

	// Records exposure, bloom and the final full-screen pass into Inputs.OutputRtv (which
	// must already be in RENDER_TARGET state). HdrColor is left in NON_PIXEL_SHADER_RESOURCE.
	void Execute (CRenderContext& Context, const SPostProcessInputs& Inputs);

	// Exposure multiplier last read back (two frames old), for the UI and for pre-exposure.
	float GetLastExposure () const;

	// Bindless raw SRV of the GPU exposure buffer (layout: Shared/PostProcessData.h,
	// LoadExposure()), for passes that want this frame's (after Execute) or last frame's
	// (before Execute) exposure without a CPU round trip. Between Execute calls the buffer is in
	// a read state that includes NON_PIXEL_SHADER_RESOURCE. FRT_INVALID_INDEX before Initialize.
	uint32 GetExposureBufferSrv () const;

	void DrawSettingsUi (SPostProcessSettings& Edit);

	SPostProcessSettings Settings;

private:
	struct SImpl;
	SImpl* Impl = nullptr;
};
}
