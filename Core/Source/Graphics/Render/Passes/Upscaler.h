#pragma once

#include <memory>
#include <string_view>

#include "Graphics/Render/RenderContext.h"


namespace frt::graphics
{
/**
 * Temporal anti-aliasing / upscaling behind a vendor-neutral interface.
 *
 * The renderer renders at RenderSize (= DisplaySize * scale), jitters the projection with the
 * sequence the active upscaler asks for, and hands the upscaler HDR colour + depth + motion
 * vectors. In-house implementation: CTemporalUpscaler (TAA at scale 1, TAAU below 1). DLSS,
 * FSR and XeSS can be added as further IUpscaler implementations without touching the
 * renderer: they consume exactly these inputs (jitter in pixels, MVs in render pixels as
 * previous-minus-current, linear view depth).
 */
enum class EUpscalerKind : uint8
{
	None = 0,        // bilinear resample, no temporal accumulation
	Temporal,        // in-house TAA / TAAU
	Dlss,            // reserved (NVIDIA Streamline / NGX)
	Fsr,             // reserved (AMD FidelityFX FSR 3.1)
	Xess,            // reserved (Intel XeSS)
	Count
};

struct SUpscalerInputs
{
	rhi::SGpuTexture* Color = nullptr;          // HDR, render resolution, RGBA16F
	rhi::SGpuTexture* ViewZ = nullptr;          // R32F linear view depth (>= 1e5 = sky)
	rhi::SGpuTexture* MotionVectors = nullptr;  // RG16F, previous - current, render pixels
	float JitterX = 0.0f;                       // render pixels, the jitter used this frame
	float JitterY = 0.0f;
	bool bResetHistory = false;
	float Exposure = 1.0f;                      // for tonemapped-space history weighting
};

class FRT_CORE_API IUpscaler
{
public:
	virtual ~IUpscaler () = default;

	virtual EUpscalerKind GetKind () const = 0;
	virtual std::string_view GetName () const = 0;
	virtual bool IsAvailable () const = 0;

	virtual void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines) = 0;
	virtual void Shutdown (rhi::CDevice& Device) = 0;
	virtual void Resize (rhi::CDevice& Device, uint32 RenderWidth, uint32 RenderHeight, uint32 DisplayWidth, uint32 DisplayHeight) = 0;

	// Writes the display-resolution HDR result into Output (RGBA16F, UAV-capable, DisplaySize).
	virtual void Execute (CRenderContext& Context, const SUpscalerInputs& Inputs, rhi::SGpuTexture& Output) = 0;

	// Sub-pixel jitter sequence (render pixels in [-0.5, 0.5]) for frame Index.
	virtual void GetJitter (uint64 FrameIndex, uint32 RenderWidth, uint32 DisplayWidth, float& OutX, float& OutY) const = 0;

	// Texture LOD bias the renderer should apply when rendering below display resolution.
	virtual float GetMipBias (uint32 RenderWidth, uint32 DisplayWidth) const = 0;

	virtual void DrawSettingsUi () {}
};

struct STemporalUpscalerSettings
{
	float HistoryBlend = 0.06f;            // base weight of the current frame
	float Sharpness = 0.25f;               // post-resolve sharpening (RCAS-like), 0 = off
	bool bCatmullRomHistory = true;        // 5-tap bicubic history fetch
	bool bVarianceClip = true;             // YCoCg variance clipping (vs. min/max clamp)
	float VarianceGamma = 1.25f;           // rectification slack in spatial standard deviations
	// Rectification slack in temporal standard deviations of the pixel's own input. Lets a
	// noisy or jitter-aliased pixel keep a history whose mean its neighbourhood rarely shows
	// (path-traced input, sub-pixel highlights); 0 = classic variance clipping.
	float TemporalGamma = 3.0f;
	// Karis 1 / (1 + L) blend weights. Off by default: on a path-traced (sparse) input they
	// pull the history toward the harmonic mean and lose energy (20% on the default view).
	bool bAntiFlicker = false;
	uint32 JitterPhases = 16u;             // Halton(2,3) sequence length (scaled by upscale ratio)

	bool bLanczosKernel = true;            // Lanczos-2 reconstruction (false = Blackman-Harris, softer)
	float KernelWidth = 1.0f;              // > 1 widens the reconstruction kernel (softer, steadier)
	float MotionHistoryScale = 0.5f;       // history cap multiplier at >= MotionSpeedRange
	float MotionSpeedRange = 32.0f;        // display pixels / frame
	bool bDepthTest = true;                // depth-reprojection disocclusion test
	float DepthTolerance = 0.03f;          // relative view-depth tolerance of that test
	bool bSharpenDenoise = true;           // RCAS noise attenuation (do not sharpen residual noise)
	float MipBiasOffset = -1.0f;           // GetMipBias = log2(Render/Display) + this, clamped
	uint32 DebugMode = 0u;                 // gpu::UpscalerDebug_* (Shared/UpscalerData.h)
};

class FRT_CORE_API CTemporalUpscaler final : public IUpscaler
{
public:
	EUpscalerKind GetKind () const override { return EUpscalerKind::Temporal; }
	std::string_view GetName () const override { return "TAAU (in-house)"; }
	bool IsAvailable () const override { return true; }

	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines) override;
	void Shutdown (rhi::CDevice& Device) override;
	void Resize (rhi::CDevice& Device, uint32 RenderWidth, uint32 RenderHeight, uint32 DisplayWidth, uint32 DisplayHeight) override;
	void Execute (CRenderContext& Context, const SUpscalerInputs& Inputs, rhi::SGpuTexture& Output) override;
	void GetJitter (uint64 FrameIndex, uint32 RenderWidth, uint32 DisplayWidth, float& OutX, float& OutY) const override;
	float GetMipBias (uint32 RenderWidth, uint32 DisplayWidth) const override;
	void DrawSettingsUi () override;

	STemporalUpscalerSettings Settings;

private:
	struct SImpl;
	SImpl* Impl = nullptr;
};

// No temporal accumulation: bilinear (or point at scale 1) resample to display size.
class FRT_CORE_API CPassthroughUpscaler final : public IUpscaler
{
public:
	EUpscalerKind GetKind () const override { return EUpscalerKind::None; }
	std::string_view GetName () const override { return "Off (spatial resample)"; }
	bool IsAvailable () const override { return true; }

	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines) override;
	void Shutdown (rhi::CDevice& Device) override {}
	void Resize (rhi::CDevice& Device, uint32 RenderWidth, uint32 RenderHeight, uint32 DisplayWidth, uint32 DisplayHeight) override {}
	void Execute (CRenderContext& Context, const SUpscalerInputs& Inputs, rhi::SGpuTexture& Output) override;
	void GetJitter (uint64 FrameIndex, uint32 RenderWidth, uint32 DisplayWidth, float& OutX, float& OutY) const override { OutX = 0.0f; OutY = 0.0f; }
	float GetMipBias (uint32 RenderWidth, uint32 DisplayWidth) const override { return 0.0f; }

private:
	rhi::HPipeline Resample = rhi::InvalidPipelineHandle;
};

// Factory for the kinds above; returns nullptr for kinds without an implementation in this build.
FRT_CORE_API std::unique_ptr<IUpscaler> CreateUpscaler (EUpscalerKind Kind);
FRT_CORE_API std::string_view GetUpscalerKindName (EUpscalerKind Kind);
}
