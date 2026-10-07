#pragma once

#include "Graphics/Render/RenderContext.h"


namespace frt::graphics
{
enum class EToneMapper : uint8
{
	AgX = 0,          // Troy Sobotka's AgX (default; graceful highlight desaturation)
	AcesFitted,       // Hill ACES RRT+ODT fit
	KhronosNeutral,   // Khronos PBR Neutral (preserves base colours, product shots)
	Reinhard,         // luminance Reinhard (reference/debug)
	Linear,           // clamp only (debug)
	Count
};

enum class EAgXLook : uint8
{
	Base = 0,
	Punchy,
	Golden,
	Count
};

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
struct SPostProcessSettings
{
	// Exposure
	bool bAutoExposure = true;
	float ExposureCompensationEV = 0.0f;
	float ManualExposureEV = 0.0f;       // used when auto exposure is off (EV100-style: L * 2^-EV)
	float MinEV = -16.0f;                // engine light units are relative: enclosed interiors meter very low
	float MaxEV = 20.0f;
	float AdaptationSpeedUp = 3.0f;      // EV per second toward brighter
	float AdaptationSpeedDown = 1.5f;    // toward darker
	// Metered window of the (4x4 block mean) log-luminance histogram. The darkest 30% (deep
	// shadow, where a path-traced estimate is mostly noise) and the brightest 10% (light
	// sources, windows, sun patches) do not steer the exposure. 50-95% exposed the Sponza
	// atrium for its sunlit walls: mean display luma 0.26, 15% of the frame crushed to black;
	// 30-90%: 0.35, 7% crushed, 3% clipped (the window).
	float HistogramLowPercentile = 0.30f;
	float HistogramHighPercentile = 0.90f;
	float AdaptationTransitionEV = 1.5f; // below this distance the linear adaptation eases out exponentially
	float MeteringCenterWeight = 0.6f;   // 0 = average metering, 1 = full centre spot
	float MeteringSpotSize = 0.5f;       // centre spot Gaussian sigma, in half screen heights

	// Bloom: a lens / eye point-spread function. Intensity is the fraction of the light scattered
	// out of the image point (energy conserving: the image is lerped toward the bloom). The
	// pyramid levels are blurs of doubling radius; level i gets weight Falloff^i, so the
	// scattered energy per octave of radius falls by Falloff and the PSF tail goes as
	// r^(log2(Falloff) - 2): 0.5 -> r^-3, like measured veiling glare (Spencer et al. 1995).
	// Falloff 1 (the previous behaviour) puts as much energy at 100 px as at 2 px: a window
	// 11 EV above the interior washed out everything within ~150 px.
	bool bBloom = true;
	float BloomIntensity = 0.04f;
	float BloomFalloff = 0.5f;           // energy ratio between successive pyramid levels (0.1 .. 1)
	float BloomRadius = 1.0f;            // upsample filter radius scale
	uint32 BloomMips = 6u;

	// Tone mapping / grading
	EToneMapper ToneMapper = EToneMapper::AgX;
	EAgXLook AgXLook = EAgXLook::Base;
	float Saturation = 1.0f;
	float Contrast = 1.0f;
	float WhiteBalanceTemperature = 6500.0f;  // scene illuminant to neutralise (lower = cooler image)
	float WhiteBalanceTint = 0.0f;            // [-1, 1], + = magenta (removes a green cast)
	bool bDither = true;
	float VignetteIntensity = 0.0f;
	float ReinhardWhitePoint = 0.0f;          // exposed luminance mapped to white; 0 = plain Reinhard
	float HdrPaperWhiteNits = 200.0f;         // FP16 (scRGB) output only
	// Linear debug views (SPostProcessInputs::bDebugIsLinear): false = show the values exactly
	// (sRGB OETF only; albedo / normals / ids), true = apply exposure + the tone mapper (radiance
	// views such as direct / indirect only). Never bloom or grading.
	bool bDebugToneMap = false;
};

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

	void DrawSettingsUi ();

	SPostProcessSettings Settings;

private:
	struct SImpl;
	SImpl* Impl = nullptr;
};
}
