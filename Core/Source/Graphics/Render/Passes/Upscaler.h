#pragma once

#include <memory>

#include "Containers/StringView.h"
#include "Graphics/Render/RenderContext.h"
#include "Graphics/Render/RenderSettings.h"

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
	virtual FStringLiteral GetName () const = 0;
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

	// Each implementation applies / edits its own part of the profile.
	virtual void ApplyProfile (const SRenderProfile& /*Profile*/) {}
	virtual void DrawSettingsUi (SRenderProfile& /*Profile*/) {}
};

class FRT_CORE_API CTemporalUpscaler final : public IUpscaler
{
public:
	EUpscalerKind GetKind () const override { return EUpscalerKind::Temporal; }
	FStringLiteral GetName () const override { return "TAAU (in-house)"; }
	bool IsAvailable () const override { return true; }

	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines) override;
	void Shutdown (rhi::CDevice& Device) override;
	void Resize (rhi::CDevice& Device, uint32 RenderWidth, uint32 RenderHeight, uint32 DisplayWidth, uint32 DisplayHeight) override;
	void Execute (CRenderContext& Context, const SUpscalerInputs& Inputs, rhi::SGpuTexture& Output) override;
	void GetJitter (uint64 FrameIndex, uint32 RenderWidth, uint32 DisplayWidth, float& OutX, float& OutY) const override;
	float GetMipBias (uint32 RenderWidth, uint32 DisplayWidth) const override;
	void ApplyProfile (const SRenderProfile& Profile) override { Settings = Profile.Taa; }
	void DrawSettingsUi (SRenderProfile& Profile) override;

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
	FStringLiteral GetName () const override { return "Off (spatial resample)"; }
	bool IsAvailable () const override { return true; }

	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines) override;
	void Shutdown (rhi::CDevice& /*Device*/) override {}
	void Resize (rhi::CDevice& /*Device*/, uint32 /*RenderWidth*/, uint32 /*RenderHeight*/, uint32 /*DisplayWidth*/, uint32 /*DisplayHeight*/) override {}
	void Execute (CRenderContext& Context, const SUpscalerInputs& Inputs, rhi::SGpuTexture& Output) override;
	void GetJitter (uint64 /*FrameIndex*/, uint32 /*RenderWidth*/, uint32 /*DisplayWidth*/, float& OutX, float& OutY) const override { OutX = 0.0f; OutY = 0.0f; }
	float GetMipBias (uint32 /*RenderWidth*/, uint32 /*DisplayWidth*/) const override { return 0.0f; }

private:
	rhi::HPipeline Resample = rhi::InvalidPipelineHandle;
};

// Factory for the kinds above; returns nullptr for kinds without an implementation in this build.
FRT_CORE_API std::unique_ptr<IUpscaler> CreateUpscaler (EUpscalerKind Kind);
FRT_CORE_API FStringLiteral GetUpscalerKindName (EUpscalerKind Kind);
}
