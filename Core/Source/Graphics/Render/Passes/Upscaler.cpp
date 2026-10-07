#include "Graphics/Render/Passes/Upscaler.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"
#include "Profiler/Profiler.h"
#include "Shared/UpscalerData.h"


namespace frt::graphics
{
namespace
{
// Kernel compression on accumulated pixels: FSR 2 uses min(Display/Render, 1.99), i.e. at a
// 2x upscale the Lanczos-2 support shrinks from 2 render pixels to ~1 render pixel (= 2
// display pixels), so every frame only contributes near its own jittered samples.
constexpr float MaxKernelScale = 1.99f;

// GetMipBias clamp. Below -3 even long accumulation cannot resolve the extra texture detail
// (shimmer); above +1 only matters for supersampling, where one mip coarser is the most a
// render/display ratio <= 2 calls for.
constexpr float MinMipBias = -3.0f;
constexpr float MaxMipBias = 1.0f;

constexpr uint32 MaxJitterPhases = 1024u;

constexpr D3D12_RESOURCE_STATES ShaderReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Radical inverse of Index in Base (Halton 1964).
float Halton (uint32 Index, uint32 Base)
{
	float fraction = 1.0f;
	float result = 0.0f;
	while (Index > 0u)
	{
		fraction /= static_cast<float>(Base);
		result += fraction * static_cast<float>(Index % Base);
		Index /= Base;
	}
	return result;
}

// FSR 2 / DLSS guidance: a render pixel covers (Display/Render)^2 display pixels, and the
// jitter sequence has to visit all of them, so its length scales with the area ratio.
uint32 ComputeJitterPhaseCount (uint32 BasePhases, uint32 RenderWidth, uint32 DisplayWidth)
{
	const float ratio = (RenderWidth > 0u && DisplayWidth > RenderWidth)
		? static_cast<float>(DisplayWidth) / static_cast<float>(RenderWidth)
		: 1.0f;
	const float phases = static_cast<float>((std::max)(BasePhases, 1u)) * ratio * ratio;
	return std::clamp(static_cast<uint32>(std::lround(phases)), 1u, MaxJitterPhases);
}

float SanitizeExposure (float Exposure)
{
	return (std::isfinite(Exposure) && Exposure > 0.0f) ? std::clamp(Exposure, 1e-6f, 1e6f) : 1.0f;
}

float SanitizeJitter (float Jitter)
{
	return std::isfinite(Jitter) ? std::clamp(Jitter, -0.5f, 0.5f) : 0.0f;
}

bool HasSrv (const rhi::SGpuTexture* Texture)
{
	return Texture && Texture->IsValid() && Texture->Srv != rhi::InvalidDescriptorIndex;
}

template <typename T>
void PushRootConstants (CRenderContext& Context, const T& Constants)
{
	static_assert(sizeof(T) % sizeof(uint32) == 0u, "root constants are whole DWORDs");
	static_assert(sizeof(T) / sizeof(uint32) <= rhi::RootConstantCount, "root constants exceed b0");
	Context.SetRootConstants(reinterpret_cast<const uint32*>(&Constants), static_cast<uint32>(sizeof(T) / sizeof(uint32)));
}

// Reconstruction kernels of TemporalResolve.hlsl (LanczosKernel / BlackmanHarrisKernel), X2 = squared distance in
// kernel units. Must stay in sync with the shader.
float EvaluateReconstructionKernel (bool bLanczos, float X2)
{
	constexpr float pi = 3.14159265358979f;
	if (bLanczos)
	{
		const float x2 = std::clamp(X2, 1e-6f, 4.0f);
		const float s = std::sin(0.5f * pi * std::sqrt(x2));
		const float c = std::cos(0.5f * pi * std::sqrt(x2));
		return 4.0f * s * s * c / (pi * pi * x2);
	}
	return std::exp(-2.29f * X2);
}

// Expected per-frame confidence E[saturate(sum w)] of an accumulated pixel. The expectation is
// over the sub-pixel phase between the display pixel centre and its nearest render sample,
// which the Halton jitter makes uniform on [-0.5, 0.5]^2. The shader mirrors this exactly:
// a 3x3 of samples at (t + q) * KernelScale, and confidence 0 when sum w <= 1e-4.
//
// Why: the cap is W_max = (1/HistoryBlend - 1) * E[c], so that the mean blend factor
// c / (W + c) equals HistoryBlend (at native; with E[c] of 1x when upscaling, the blend per
// sample stays the same instead, see Execute). The previous closed form E[c] ~ 1/(kx ky) is
// wrong for both kernels. Radial Lanczos-2 sums to only 0.80 at 1x (its negative lobes do not
// form a partition of unity) and Blackman-Harris to 0.35 instead of 0.25 at 2x. Simulated
// with the running-mean recurrence, the effective blend was 0.045 to 0.080 for a requested
// 0.06. With this it is within 5% everywhere.
float ComputeMeanConfidence (bool bLanczos, float KernelScaleX, float KernelScaleY)
{
	constexpr int grid = 16;
	double sum = 0.0;
	for (int gy = 0; gy < grid; ++gy)
	{
		const float qy = (static_cast<float>(gy) + 0.5f) / static_cast<float>(grid) - 0.5f;
		for (int gx = 0; gx < grid; ++gx)
		{
			const float qx = (static_cast<float>(gx) + 0.5f) / static_cast<float>(grid) - 0.5f;
			float weightSum = 0.0f;
			for (int ty = -1; ty <= 1; ++ty)
			{
				for (int tx = -1; tx <= 1; ++tx)
				{
					const float ox = (static_cast<float>(tx) + qx) * KernelScaleX;
					const float oy = (static_cast<float>(ty) + qy) * KernelScaleY;
					weightSum += EvaluateReconstructionKernel(bLanczos, ox * ox + oy * oy);
				}
			}
			sum += weightSum > 1e-4f ? std::clamp(weightSum, 0.0f, 1.0f) : 0.0f;
		}
	}
	return (std::max)(static_cast<float>(sum / static_cast<double>(grid * grid)), 1e-3f);
}

void RecordResample (CRenderContext& Context, rhi::HPipeline Pipeline, const rhi::SGpuTexture& Input, rhi::SGpuTexture& Output)
{
	gpu::SUpscalerResampleConstants constants = {};
	constants.Input = Input.Srv;
	constants.Output = Output.Uav;
	constants.OutputSize = { Output.Width, Output.Height };
	constants.InvOutputSize = { 1.0f / static_cast<float>((std::max)(Output.Width, 1u)), 1.0f / static_cast<float>((std::max)(Output.Height, 1u)) };
	constants.InputSize = { Input.Width, Input.Height };

	PushRootConstants(Context, constants);
	Context.Dispatch2D(Pipeline, Output.Width, Output.Height);
}

rhi::SGpuTexture CreateHistoryTexture (rhi::CDevice& Device, uint32 Width, uint32 Height, const wchar_t* Name)
{
	rhi::STextureDesc desc;
	desc.Width = Width;
	desc.Height = Height;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.bSrv = true;
	desc.bUav = true;
	desc.Name = Name;
	return Device.CreateTexture(desc);
}
}


// ---------------------------------------------------------------------------------------------
// CTemporalUpscaler
// ---------------------------------------------------------------------------------------------

struct CTemporalUpscaler::SImpl
{
	rhi::HPipeline Resolve = rhi::InvalidPipelineHandle;
	rhi::HPipeline Sharpen = rhi::InvalidPipelineHandle;
	// Spatial resample, used only while the resolve has no PSO (initial compile error), so
	// Output is still written as the IUpscaler contract requires.
	rhi::HPipeline Fallback = rhi::InvalidPipelineHandle;

	// Cache of ComputeMeanConfidence for both kernels (re-evaluated only when the scale changes),
	// at the kernel scale of this frame and at the scale the same KernelWidth gives at 1x.
	float CachedKernelScaleX = -1.0f;
	float CachedKernelScaleY = -1.0f;
	float CachedNativeKernelScale = -1.0f;
	float CachedMeanConfidenceLanczos = 1.0f;
	float CachedMeanConfidenceGaussian = 1.0f;
	float CachedNativeConfidenceLanczos = 1.0f;
	float CachedNativeConfidenceGaussian = 1.0f;

	// Display-resolution ping-pong: rgb = linear HDR (input space), a = accumulated confidence.
	rhi::SGpuTexture History[2];
	// Same ping-pong index: temporal moments of the working-space input, rgb = E[w], a = Var[w.Y]
	// (TemporalResolve.hlsl, rectification).
	rhi::SGpuTexture HistoryMoments[2];
	uint32 Latest = 0u;                 // History[Latest] holds the most recent resolve

	// Copy of last frame's ViewZ for the depth-reprojection disocclusion test.
	rhi::SGpuTexture PrevViewZ;

	uint32 RenderWidth = 0u;
	uint32 RenderHeight = 0u;
	uint32 DisplayWidth = 0u;
	uint32 DisplayHeight = 0u;

	bool bHistoryValid = false;
	bool bPrevViewZValid = false;
	bool bHasExecuted = false;
	bool bResetRequested = false;
	uint64 LastFrameNumber = 0u;
	float PrevJitterX = 0.0f;
	float PrevJitterY = 0.0f;

	// Values of the last dispatch, for the UI.
	float LastMaxHistoryWeight = 0.0f;
	float LastKernelScale = 1.0f;
};

void CTemporalUpscaler::Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines)
{
	(void)Device;
	if (!Impl)
	{
		Impl = new SImpl();
	}
	if (Impl->Resolve == rhi::InvalidPipelineHandle)
	{
		Impl->Resolve = Pipelines.CreateCompute("Upscaler_TemporalResolve", MakeShaderDesc("Upscaler/TemporalResolve.hlsl", "main", "cs_6_6"));
	}
	if (Impl->Sharpen == rhi::InvalidPipelineHandle)
	{
		Impl->Sharpen = Pipelines.CreateCompute("Upscaler_Sharpen", MakeShaderDesc("Upscaler/Sharpen.hlsl", "main", "cs_6_6"));
	}
	// Impl->Fallback is created lazily by Execute, only if the resolve has no PSO: it would
	// otherwise be compiled for nothing in the common case.
}

void CTemporalUpscaler::Shutdown (rhi::CDevice& Device)
{
	if (!Impl)
	{
		return;
	}
	for (rhi::SGpuTexture& history : Impl->History)
	{
		Device.Destroy(history);
	}
	for (rhi::SGpuTexture& moments : Impl->HistoryMoments)
	{
		Device.Destroy(moments);
	}
	Device.Destroy(Impl->PrevViewZ);
	delete Impl;
	Impl = nullptr;
}

void CTemporalUpscaler::Resize (rhi::CDevice& Device, uint32 RenderWidth, uint32 RenderHeight, uint32 DisplayWidth, uint32 DisplayHeight)
{
	if (!Impl)
	{
		return;
	}
	SImpl& impl = *Impl;
	RenderWidth = (std::max)(RenderWidth, 1u);
	RenderHeight = (std::max)(RenderHeight, 1u);
	DisplayWidth = (std::max)(DisplayWidth, 1u);
	DisplayHeight = (std::max)(DisplayHeight, 1u);

	// The history lives in display space, so a render-resolution change alone (dynamic
	// resolution, scale slider) keeps it: only the display size invalidates it.
	if (DisplayWidth != impl.DisplayWidth || DisplayHeight != impl.DisplayHeight
		|| !impl.History[0].IsValid() || !impl.History[1].IsValid()
		|| !impl.HistoryMoments[0].IsValid() || !impl.HistoryMoments[1].IsValid())
	{
		for (rhi::SGpuTexture& history : impl.History)
		{
			Device.Destroy(history);
		}
		for (rhi::SGpuTexture& moments : impl.HistoryMoments)
		{
			Device.Destroy(moments);
		}
		impl.History[0] = CreateHistoryTexture(Device, DisplayWidth, DisplayHeight, L"Upscaler.History0");
		impl.History[1] = CreateHistoryTexture(Device, DisplayWidth, DisplayHeight, L"Upscaler.History1");
		impl.HistoryMoments[0] = CreateHistoryTexture(Device, DisplayWidth, DisplayHeight, L"Upscaler.HistoryMoments0");
		impl.HistoryMoments[1] = CreateHistoryTexture(Device, DisplayWidth, DisplayHeight, L"Upscaler.HistoryMoments1");
		impl.Latest = 0u;
		impl.bHistoryValid = false;
	}

	// Previous depth is render resolution; recreated lazily in Execute with ViewZ's format.
	if (RenderWidth != impl.RenderWidth || RenderHeight != impl.RenderHeight)
	{
		Device.Destroy(impl.PrevViewZ);
		impl.bPrevViewZValid = false;
	}

	impl.RenderWidth = RenderWidth;
	impl.RenderHeight = RenderHeight;
	impl.DisplayWidth = DisplayWidth;
	impl.DisplayHeight = DisplayHeight;
}

void CTemporalUpscaler::Execute (CRenderContext& Context, const SUpscalerInputs& Inputs, rhi::SGpuTexture& Output)
{
	if (!Impl || !Context.Device || !Context.Pipelines || !Context.CommandList
		|| !HasSrv(Inputs.Color) || !HasSrv(Inputs.ViewZ) || !HasSrv(Inputs.MotionVectors)
		|| !Output.IsValid() || Output.Uav == rhi::InvalidDescriptorIndex)
	{
		return;
	}
	SImpl& impl = *Impl;
	rhi::CDevice& device = *Context.Device;

	if (!Context.Pipelines->Get(impl.Resolve))
	{
		// The resolve failed its initial compile (hot reload keeps retrying). Output still has
		// to be written, or post would present an uninitialised / stale DisplayColor.
		impl.bHistoryValid = false;
		if (impl.Fallback == rhi::InvalidPipelineHandle)
		{
			impl.Fallback = Context.Pipelines->CreateCompute("Upscaler_ResolveFallback", MakeShaderDesc("Upscaler/Resample.hlsl", "main", "cs_6_6"));
		}
		Context.BindCompute();
		FRT_GPU_SCOPE(Context.CommandList, "Upscaler_ResolveFallback");
		Context.Barriers.Transition(*Inputs.Color, ShaderReadState);
		Context.Barriers.Transition(Output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		RecordResample(Context, impl.Fallback, *Inputs.Color, Output);
		return;
	}

	// The textures are the source of truth for the sizes; a missed Resize must not make the
	// dispatch read or write out of bounds.
	const uint32 renderWidth = Inputs.Color->Width;
	const uint32 renderHeight = Inputs.Color->Height;
	const uint32 displayWidth = Output.Width;
	const uint32 displayHeight = Output.Height;
	if (renderWidth != impl.RenderWidth || renderHeight != impl.RenderHeight
		|| displayWidth != impl.DisplayWidth || displayHeight != impl.DisplayHeight
		|| !impl.History[0].IsValid() || !impl.History[1].IsValid()
		|| !impl.HistoryMoments[0].IsValid() || !impl.HistoryMoments[1].IsValid())
	{
		Resize(device, renderWidth, renderHeight, displayWidth, displayHeight);
	}

	const STemporalUpscalerSettings& settings = Settings;

	// The shader addresses Color, ViewZ and MotionVectors with the same texel coordinates. A
	// mismatched G-buffer (e.g. a pass resized without the others) would reproject with
	// out-of-bounds MVs (zero, i.e. ghosting). Degrade to a spatial-only upscale instead.
	const bool bInputsConsistent =
		Inputs.ViewZ->Width == renderWidth && Inputs.ViewZ->Height == renderHeight
		&& Inputs.MotionVectors->Width == renderWidth && Inputs.MotionVectors->Height == renderHeight;

	// CopyResource needs an identical twin of ViewZ.
	if (settings.bDepthTest && bInputsConsistent
		&& (!impl.PrevViewZ.IsValid() || impl.PrevViewZ.Width != Inputs.ViewZ->Width
			|| impl.PrevViewZ.Height != Inputs.ViewZ->Height || impl.PrevViewZ.Format != Inputs.ViewZ->Format
			|| impl.PrevViewZ.MipLevels != Inputs.ViewZ->MipLevels))
	{
		device.Destroy(impl.PrevViewZ);
		rhi::STextureDesc desc;
		desc.Width = Inputs.ViewZ->Width;
		desc.Height = Inputs.ViewZ->Height;
		desc.MipLevels = Inputs.ViewZ->MipLevels;
		desc.Format = Inputs.ViewZ->Format;
		desc.SrvFormat = Inputs.ViewZ->Format == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN;
		desc.bSrv = true;
		desc.Name = L"Upscaler.PrevViewZ";
		impl.PrevViewZ = device.CreateTexture(desc);
		impl.bPrevViewZValid = false;
	}

	// History is stale after a camera cut, a skipped frame (upscaler switched off and on, mode
	// change) or a frame counter restart.
	bool bReset = Inputs.bResetHistory || impl.bResetRequested || !impl.bHistoryValid || !bInputsConsistent;
	if (impl.bHasExecuted && (Context.FrameNumber > impl.LastFrameNumber + 1u || Context.FrameNumber < impl.LastFrameNumber))
	{
		bReset = true;
	}
	impl.bResetRequested = false;

	const float exposure = SanitizeExposure(Inputs.Exposure);
	const float jitterX = SanitizeJitter(Inputs.JitterX);
	const float jitterY = SanitizeJitter(Inputs.JitterY);

	// Kernel compression of accumulated pixels (see MaxKernelScale), optionally widened.
	const float ratioX = static_cast<float>(displayWidth) / static_cast<float>(renderWidth);
	const float ratioY = static_cast<float>(displayHeight) / static_cast<float>(renderHeight);
	const float kernelWidth = std::isfinite(settings.KernelWidth) ? std::clamp(settings.KernelWidth, 0.5f, 2.0f) : 1.0f;
	const float kernelScaleX = std::clamp(std::clamp(ratioX, 1.0f, MaxKernelScale) / kernelWidth, 1.0f, 2.0f);
	const float kernelScaleY = std::clamp(std::clamp(ratioY, 1.0f, MaxKernelScale) / kernelWidth, 1.0f, 2.0f);

	// The blend is a running confidence-weighted mean, alpha = c / (W + c), with W the
	// accumulated confidence capped at W_max. With W at the cap, the mean blend is
	// E[c] / (W_max + E[c]). So W_max = (1/HistoryBlend - 1) * E[c] makes the average frame enter
	// with HistoryBlend, i.e. a temporal window of ~1/HistoryBlend frames at native resolution. Within that window, samples that land close to a display pixel (c ~ 1) still
	// count more than distant ones (c ~ 0). E[c] is integrated numerically
	// (ComputeMeanConfidence).
	//
	// Upscaling keeps the window constant in SAMPLES, not frames: the cap uses E[c] of the same
	// kernel at 1x, so a display pixel averages as many nearby samples at 2x as at native (the
	// frame window grows ~ratio^2, like the jitter cycle it has to cover). A 16-frame window at
	// 2x held only ~4.7 effective samples per display pixel against a 64-phase jitter cycle;
	// pixels never converged and froze in a 2-pixel checker pattern. Motion still shortens the
	// window (MotionHistoryScale) and the rectification catches change.
	//
	// The shader blends per pixel between Lanczos-2 (clean pixels) and Blackman-Harris (noisy
	// ones), so it gets the cap for both.
	const bool bLanczos = settings.bLanczosKernel;
	const float nativeKernelScale = std::clamp(1.0f / kernelWidth, 1.0f, 2.0f);
	if (impl.CachedKernelScaleX != kernelScaleX || impl.CachedKernelScaleY != kernelScaleY || impl.CachedNativeKernelScale != nativeKernelScale)
	{
		impl.CachedKernelScaleX = kernelScaleX;
		impl.CachedKernelScaleY = kernelScaleY;
		impl.CachedNativeKernelScale = nativeKernelScale;
		impl.CachedMeanConfidenceLanczos = ComputeMeanConfidence(true, kernelScaleX, kernelScaleY);
		impl.CachedMeanConfidenceGaussian = ComputeMeanConfidence(false, kernelScaleX, kernelScaleY);
		impl.CachedNativeConfidenceLanczos = ComputeMeanConfidence(true, nativeKernelScale, nativeKernelScale);
		impl.CachedNativeConfidenceGaussian = ComputeMeanConfidence(false, nativeKernelScale, nativeKernelScale);
	}
	const float historyBlend = std::isfinite(settings.HistoryBlend) ? std::clamp(settings.HistoryBlend, 0.005f, 1.0f) : 0.06f;
	const float maxHistoryWeightGaussian = (1.0f / historyBlend - 1.0f) * impl.CachedNativeConfidenceGaussian;
	const float maxHistoryWeight = bLanczos ? (1.0f / historyBlend - 1.0f) * impl.CachedNativeConfidenceLanczos : maxHistoryWeightGaussian;

	const bool bDebug = settings.DebugMode != gpu::UpscalerDebug_None && settings.DebugMode < gpu::UpscalerDebug_Count;
	const bool bSharpen = !bDebug && std::isfinite(settings.Sharpness) && settings.Sharpness > 0.0f
		&& Context.Pipelines->Get(impl.Sharpen) != nullptr;
	const bool bUseDepthTest = settings.bDepthTest && bInputsConsistent && impl.PrevViewZ.IsValid();

	rhi::SGpuTexture& historyIn = impl.History[impl.Latest];
	rhi::SGpuTexture& historyOut = impl.History[impl.Latest ^ 1u];
	rhi::SGpuTexture& momentsIn = impl.HistoryMoments[impl.Latest];
	rhi::SGpuTexture& momentsOut = impl.HistoryMoments[impl.Latest ^ 1u];

	Context.BindCompute();

	{
		FRT_GPU_SCOPE(Context.CommandList, "Upscaler_Resolve");

		Context.Barriers.Transition(*Inputs.Color, ShaderReadState);
		Context.Barriers.Transition(*Inputs.ViewZ, ShaderReadState);
		Context.Barriers.Transition(*Inputs.MotionVectors, ShaderReadState);
		Context.Barriers.Transition(historyIn, ShaderReadState);
		Context.Barriers.Transition(historyOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Context.Barriers.Transition(momentsIn, ShaderReadState);
		Context.Barriers.Transition(momentsOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (impl.PrevViewZ.IsValid())
		{
			Context.Barriers.Transition(impl.PrevViewZ, ShaderReadState);
		}
		if (!bSharpen)
		{
			Context.Barriers.Transition(Output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}

		uint32 flags = 0u;
		if (!bReset)
		{
			flags |= gpu::UpscalerFlag_HistoryValid;
		}
		if (bUseDepthTest && impl.bPrevViewZValid)
		{
			flags |= gpu::UpscalerFlag_PrevDepthValid;
		}
		if (bUseDepthTest)
		{
			flags |= gpu::UpscalerFlag_DepthTest;
		}
		if (settings.bCatmullRomHistory)
		{
			flags |= gpu::UpscalerFlag_CatmullRom;
		}
		if (settings.bVarianceClip)
		{
			flags |= gpu::UpscalerFlag_VarianceClip;
		}
		if (settings.bAntiFlicker)
		{
			flags |= gpu::UpscalerFlag_AntiFlicker;
		}
		if (settings.bLanczosKernel)
		{
			flags |= gpu::UpscalerFlag_LanczosKernel;
		}

		gpu::STemporalUpscalerConstants constants = {};
		constants.InColor = Inputs.Color->Srv;
		constants.InViewZ = Inputs.ViewZ->Srv;
		constants.InMotionVectors = Inputs.MotionVectors->Srv;
		constants.InPrevViewZ = impl.PrevViewZ.IsValid() ? impl.PrevViewZ.Srv : FRT_INVALID_INDEX;
		constants.InHistory = historyIn.Srv;
		constants.OutHistory = historyOut.Uav;
		constants.OutColor = bSharpen ? FRT_INVALID_INDEX : Output.Uav;
		constants.Flags = flags;
		constants.RenderSize = { static_cast<float>(renderWidth), static_cast<float>(renderHeight) };
		constants.InvRenderSize = { 1.0f / static_cast<float>(renderWidth), 1.0f / static_cast<float>(renderHeight) };
		constants.DisplaySize = { static_cast<float>(displayWidth), static_cast<float>(displayHeight) };
		constants.InvDisplaySize = { 1.0f / static_cast<float>(displayWidth), 1.0f / static_cast<float>(displayHeight) };
		constants.Jitter = { jitterX, jitterY };
		constants.PrevJitter = { impl.PrevJitterX, impl.PrevJitterY };
		constants.KernelScale = { kernelScaleX, kernelScaleY };
		constants.Exposure = exposure;
		constants.InvExposure = 1.0f / exposure;
		constants.MaxHistoryWeight = maxHistoryWeight;
		constants.MotionHistoryScale = std::isfinite(settings.MotionHistoryScale) ? std::clamp(settings.MotionHistoryScale, 0.0f, 1.0f) : 1.0f;
		constants.InvMotionSpeedRange = 1.0f / (std::isfinite(settings.MotionSpeedRange) ? std::clamp(settings.MotionSpeedRange, 0.5f, 4096.0f) : 32.0f);
		constants.VarianceGamma = std::isfinite(settings.VarianceGamma) ? std::clamp(settings.VarianceGamma, 0.25f, 8.0f) : 1.25f;
		constants.DepthTolerance = std::isfinite(settings.DepthTolerance) ? std::clamp(settings.DepthTolerance, 1e-4f, 1.0f) : 0.03f;
		constants.DebugMode = bDebug ? settings.DebugMode : gpu::UpscalerDebug_None;
		constants.RenderScale = {
			static_cast<float>(static_cast<double>(renderWidth) / static_cast<double>(displayWidth)),
			static_cast<float>(static_cast<double>(renderHeight) / static_cast<double>(displayHeight)) };
		constants.InHistoryMoments = momentsIn.Srv;
		constants.OutHistoryMoments = momentsOut.Uav;
		constants.TemporalGamma = std::isfinite(settings.TemporalGamma) ? std::clamp(settings.TemporalGamma, 0.0f, 16.0f) : 3.0f;
		constants.MaxHistoryWeightGaussian = maxHistoryWeightGaussian;

		Context.PushPassConstants(constants);
		Context.Dispatch2D(impl.Resolve, displayWidth, displayHeight);
	}

	if (bSharpen)
	{
		FRT_GPU_SCOPE(Context.CommandList, "Upscaler_Sharpen");

		Context.Barriers.Transition(historyOut, ShaderReadState);
		Context.Barriers.Transition(Output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		gpu::SUpscalerSharpenConstants constants = {};
		constants.Input = historyOut.Srv;
		constants.Output = Output.Uav;
		constants.Size = { displayWidth, displayHeight };
		constants.Sharpness = (std::min)(settings.Sharpness, 1.0f);
		constants.Exposure = exposure;
		constants.InvExposure = 1.0f / exposure;
		constants.Flags = settings.bSharpenDenoise ? gpu::UpscalerSharpenFlag_Denoise : 0u;

		PushRootConstants(Context, constants);
		Context.Dispatch2D(impl.Sharpen, displayWidth, displayHeight);
	}

	// Keep this frame's depth for next frame's disocclusion test (render res R32F copy).
	if (bUseDepthTest)
	{
		FRT_GPU_SCOPE(Context.CommandList, "Upscaler_CopyDepth");

		Context.Barriers.Transition(*Inputs.ViewZ, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Context.Barriers.Transition(impl.PrevViewZ, D3D12_RESOURCE_STATE_COPY_DEST);
		Context.FlushBarriers();
		Context.CommandList->CopyResource(impl.PrevViewZ.Resource.Get(), Inputs.ViewZ->Resource.Get());
		impl.bPrevViewZValid = true;
	}
	else
	{
		impl.bPrevViewZValid = false;
	}

	impl.Latest ^= 1u;
	impl.bHistoryValid = true;
	impl.bHasExecuted = true;
	impl.LastFrameNumber = Context.FrameNumber;
	impl.PrevJitterX = jitterX;
	impl.PrevJitterY = jitterY;
	impl.LastMaxHistoryWeight = maxHistoryWeight;
	impl.LastKernelScale = kernelScaleX;
}

void CTemporalUpscaler::GetJitter (uint64 FrameIndex, uint32 RenderWidth, uint32 DisplayWidth, float& OutX, float& OutY) const
{
	const uint32 phases = ComputeJitterPhaseCount(Settings.JitterPhases, RenderWidth, DisplayWidth);
	// Halton index 0 is (0, 0) in every base; start at 1 like FSR 2.
	const uint32 index = static_cast<uint32>(FrameIndex % phases) + 1u;
	OutX = Halton(index, 2u) - 0.5f;
	OutY = Halton(index, 3u) - 0.5f;
}

// MipBias = clamp(log2(Render / Display) + MipBiasOffset, -3, +1), MipBiasOffset = -1 by
// default. log2(Render/Display) matches texture detail to the display pixel grid; the extra
// -1 is the FSR 2 / DLSS recommendation: the jittered history integrates JitterPhases samples
// per pixel, enough to resolve one mip finer than a single frame could. So native TAA gets
// -1, a 2x upscale -2. Lower MipBiasOffset (toward 0) if high-frequency textures shimmer.
float CTemporalUpscaler::GetMipBias (uint32 RenderWidth, uint32 DisplayWidth) const
{
	if (RenderWidth == 0u || DisplayWidth == 0u)
	{
		return 0.0f;
	}
	const float offset = std::isfinite(Settings.MipBiasOffset) ? Settings.MipBiasOffset : -1.0f;
	const float bias = std::log2(static_cast<float>(RenderWidth) / static_cast<float>(DisplayWidth)) + offset;
	return std::clamp(bias, MinMipBias, MaxMipBias);
}

void CTemporalUpscaler::DrawSettingsUi ()
{
	STemporalUpscalerSettings& s = Settings;
	ImGui::PushID("TemporalUpscaler");

	ImGui::SliderFloat("Current frame weight", &s.HistoryBlend, 0.01f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat("Sharpness (RCAS)", &s.Sharpness, 0.0f, 1.0f, "%.2f");
	ImGui::BeginDisabled(s.Sharpness <= 0.0f);
	ImGui::Checkbox("Sharpen: skip noise", &s.bSharpenDenoise);
	ImGui::EndDisabled();

	int kernel = s.bLanczosKernel ? 0 : 1;
	if (ImGui::Combo("Reconstruction kernel", &kernel, "Lanczos-2 (Blackman-Harris on noise)\0Blackman-Harris\0"))
	{
		s.bLanczosKernel = kernel == 0;
	}
	ImGui::SliderFloat("Kernel width", &s.KernelWidth, 0.5f, 2.0f, "%.2f");
	ImGui::Checkbox("Catmull-Rom history", &s.bCatmullRomHistory);
	ImGui::Checkbox("Variance clipping (YCoCg)", &s.bVarianceClip);
	ImGui::BeginDisabled(!s.bVarianceClip);
	ImGui::SliderFloat("Variance gamma", &s.VarianceGamma, 0.5f, 3.0f, "%.2f");
	ImGui::EndDisabled();
	ImGui::SliderFloat("Temporal gamma", &s.TemporalGamma, 0.0f, 6.0f, "%.2f");
	ImGui::SetItemTooltip("Rectification slack in temporal standard deviations of the pixel's own input. "
		"Keeps noisy / jitter-aliased pixels (path tracing, sub-pixel highlights) from being clipped to black. 0 = classic clipping.");
	ImGui::Checkbox("Anti-flicker (luminance weights)", &s.bAntiFlicker);
	ImGui::Checkbox("Depth disocclusion test", &s.bDepthTest);
	ImGui::BeginDisabled(!s.bDepthTest);
	ImGui::SliderFloat("Depth tolerance", &s.DepthTolerance, 0.001f, 0.25f, "%.3f", ImGuiSliderFlags_Logarithmic);
	ImGui::EndDisabled();
	ImGui::SliderFloat("Motion history scale", &s.MotionHistoryScale, 0.05f, 1.0f, "%.2f");
	ImGui::SliderFloat("Motion speed range (px)", &s.MotionSpeedRange, 1.0f, 128.0f, "%.0f");

	int phases = static_cast<int>(s.JitterPhases);
	if (ImGui::SliderInt("Jitter phases (at 1x)", &phases, 1, 64))
	{
		s.JitterPhases = static_cast<uint32>((std::max)(phases, 1));
	}
	ImGui::SliderFloat("Mip bias offset", &s.MipBiasOffset, -2.0f, 1.0f, "%.2f");

	int debug = static_cast<int>(s.DebugMode);
	if (ImGui::Combo("Debug view", &debug, "None\0History weight\0Rejection (r) / confidence (g) / clip (b)\0Motion vectors\0"))
	{
		s.DebugMode = static_cast<uint32>(debug);
	}

	if (Impl)
	{
		if (ImGui::Button("Reset history"))
		{
			Impl->bResetRequested = true;
		}
		const SImpl& impl = *Impl;
		const float ratio = impl.RenderWidth > 0u ? static_cast<float>(impl.DisplayWidth) / static_cast<float>(impl.RenderWidth) : 1.0f;
		ImGui::Text("Render %ux%u -> display %ux%u (%.2fx)", impl.RenderWidth, impl.RenderHeight, impl.DisplayWidth, impl.DisplayHeight, ratio);
		ImGui::Text("Jitter phases %u, mip bias %.2f", ComputeJitterPhaseCount(s.JitterPhases, impl.RenderWidth, impl.DisplayWidth),
			GetMipBias(impl.RenderWidth, impl.DisplayWidth));
		ImGui::Text("Kernel scale %.2f, mean confidence %.3f (BH %.3f), history weight cap %.2f", impl.LastKernelScale,
			impl.CachedMeanConfidenceLanczos, impl.CachedMeanConfidenceGaussian, impl.LastMaxHistoryWeight);
	}

	ImGui::PopID();
}


// ---------------------------------------------------------------------------------------------
// CPassthroughUpscaler
// ---------------------------------------------------------------------------------------------

void CPassthroughUpscaler::Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines)
{
	(void)Device;
	if (Resample == rhi::InvalidPipelineHandle)
	{
		Resample = Pipelines.CreateCompute("Upscaler_Resample", MakeShaderDesc("Upscaler/Resample.hlsl", "main", "cs_6_6"));
	}
}

void CPassthroughUpscaler::Execute (CRenderContext& Context, const SUpscalerInputs& Inputs, rhi::SGpuTexture& Output)
{
	if (!Context.CommandList || !HasSrv(Inputs.Color) || !Output.IsValid() || Output.Uav == rhi::InvalidDescriptorIndex)
	{
		return;
	}

	Context.BindCompute();
	FRT_GPU_SCOPE(Context.CommandList, "Upscaler_Resample");

	Context.Barriers.Transition(*Inputs.Color, ShaderReadState);
	Context.Barriers.Transition(Output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	RecordResample(Context, Resample, *Inputs.Color, Output);
}


// ---------------------------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------------------------

std::unique_ptr<IUpscaler> CreateUpscaler (EUpscalerKind Kind)
{
	switch (Kind)
	{
		case EUpscalerKind::None:
			return std::make_unique<CPassthroughUpscaler>();
		case EUpscalerKind::Temporal:
			return std::make_unique<CTemporalUpscaler>();
		// Extension points: each vendor SDK becomes one more IUpscaler consuming the same
		// SUpscalerInputs (jitter in render pixels, MVs = previous - current in render pixels,
		// linear ViewZ) and providing its own GetJitter / GetMipBias. Not built in yet.
		case EUpscalerKind::Dlss:   // NVIDIA Streamline / NGX DLSS-SR
		case EUpscalerKind::Fsr:    // AMD FidelityFX FSR 3.1 upscaler
		case EUpscalerKind::Xess:   // Intel XeSS SR
		default:
			return nullptr;
	}
}

std::string_view GetUpscalerKindName (EUpscalerKind Kind)
{
	switch (Kind)
	{
		case EUpscalerKind::None: return "Off (spatial resample)";
		case EUpscalerKind::Temporal: return "TAA / TAAU (in-house)";
		case EUpscalerKind::Dlss: return "DLSS";
		case EUpscalerKind::Fsr: return "FSR";
		case EUpscalerKind::Xess: return "XeSS";
		default: return "Unknown";
	}
}
}
