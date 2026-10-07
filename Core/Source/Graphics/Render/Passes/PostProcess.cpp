#include "Graphics/Render/Passes/PostProcess.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"

#include "Profiler/Profiler.h"
#include "Shared/PostProcessData.h"


namespace frt::graphics
{
namespace
{
constexpr uint32 kMaxBloomLevels = 8u;
constexpr uint32 kReadbackSlots = rhi::MaxFramesInFlight + 1u;
constexpr DXGI_FORMAT kBloomFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kDefaultOutputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr uint32 kHistogramTile = 64u;      // pixels per histogram group side (ExposureHistogram.hlsl)

// Read-only state usable by compute and pixel shaders at once.
constexpr D3D12_RESOURCE_STATES kAnyShaderRead =
	D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// Exposed values above this are clamped before the bloom pyramid. Every pyramid texel is a
// convex combination of clamped inputs (normalised 13-tap / Karis weights, tent weights summing
// to 1, running-mean upsample), so RGBA16F stays below this bound and finite with margin.
constexpr float kMaxExposedBloomInput = 32768.0f;

// Executes after an exposure reset during which the exposure keeps snapping to the metered
// target. The frame that requests the reset is the first of a new history: path-traced, it is
// one sample per pixel without temporal accumulation, nearly black, and meters 5-6 EV too dark.
// Snapping once to that and then adapting at AdaptationSpeedUp left the default view 0.65 EV
// under-exposed two seconds later. 32 frames cover the TAA / denoiser history ramp.
constexpr uint32 kExposureSettleFrames = 32u;

// The histogram spans [MinEV, MaxEV] plus this margin (EV) on both sides, so a scene pinned at a
// clamp still shows where its distribution lies.
constexpr float kHistogramMarginEV = 1.0f;

// Tint +-1 moves the assumed illuminant +-0.03 Duv off the Planckian locus (CIE 1960 uv); 0.03
// covers green fluorescent / magenta LED casts.
constexpr double kTintDuvScale = 0.03;

constexpr double kReferenceWhiteKelvin = 6500.0;

float SanitizeFloat (float Value, float Fallback, float Min, float Max)
{
	return std::isfinite(Value) ? std::clamp(Value, Min, Max) : Fallback;
}


// ---------------------------------------------------------------------------------------------
// Colour science (CPU, double precision): white balance + saturation folded into one 3x3.
// ---------------------------------------------------------------------------------------------

struct SMat3
{
	double M[3][3] = {};
};

SMat3 Multiply (const SMat3& A, const SMat3& B)
{
	SMat3 result;
	for (int r = 0; r < 3; ++r)
	{
		for (int c = 0; c < 3; ++c)
		{
			result.M[r][c] = A.M[r][0] * B.M[0][c] + A.M[r][1] * B.M[1][c] + A.M[r][2] * B.M[2][c];
		}
	}
	return result;
}

SMat3 Inverse (const SMat3& A)
{
	const auto& m = A.M;
	const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
	const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
	const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
	const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
	const double invDet = std::abs(det) > 1e-300 ? 1.0 / det : 0.0;

	SMat3 result;
	result.M[0][0] = c00 * invDet;
	result.M[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * invDet;
	result.M[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * invDet;
	result.M[1][0] = c01 * invDet;
	result.M[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * invDet;
	result.M[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * invDet;
	result.M[2][0] = c02 * invDet;
	result.M[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * invDet;
	result.M[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * invDet;
	return result;
}

SMat3 Diagonal (double X, double Y, double Z)
{
	SMat3 result;
	result.M[0][0] = X;
	result.M[1][1] = Y;
	result.M[2][2] = Z;
	return result;
}

void Transform (const SMat3& A, const double (&V)[3], double (&Out)[3])
{
	for (int r = 0; r < 3; ++r)
	{
		Out[r] = A.M[r][0] * V[0] + A.M[r][1] * V[1] + A.M[r][2] * V[2];
	}
}

// Linear Rec.709 (sRGB primaries, D65 white 0.3127 / 0.3290) -> CIE XYZ, IEC 61966-2-1.
constexpr SMat3 kRec709ToXyz = { {
	{ 0.4123907992659595, 0.3575843393838780, 0.1804807884018343 },
	{ 0.2126390058715104, 0.7151686787677559, 0.0721923153607337 },
	{ 0.0193308187155918, 0.1191947797946259, 0.9505321522496608 } } };

// Bradford cone response (Lam 1985), the CAT used by ICC v4 / Lindbloom.
constexpr SMat3 kBradford = { {
	{ 0.8951, 0.2664, -0.1614 },
	{ -0.7502, 1.7135, 0.0367 },
	{ 0.0389, -0.0685, 1.0296 } } };

// CCT -> CIE 1931 xy on the Planckian locus: Kang et al. 2002 ("Design of advanced color
// temperature control system for HDTV applications", J. Korean Phys. Soc. 41) cubic fit,
// valid 1667 K .. 25000 K.
void PlanckianLocusXy (double Kelvin, double& X, double& Y)
{
	const double t = std::clamp(Kelvin, 1667.0, 25000.0);
	const double k = 1e3 / t;
	const double k2 = k * k;
	const double k3 = k2 * k;
	X = t <= 4000.0
		? -0.2661239 * k3 - 0.2343589 * k2 + 0.8776956 * k + 0.179910
		: -3.0258469 * k3 + 2.1070379 * k2 + 0.2226347 * k + 0.240390;
	const double x2 = X * X;
	const double x3 = x2 * X;
	if (t <= 2222.0)
	{
		Y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * X - 0.20219683;
	}
	else if (t <= 4000.0)
	{
		Y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * X - 0.16748867;
	}
	else
	{
		Y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * X - 0.37001483;
	}
}

// CIE 1960 UCS, the space in which Duv (tint) is defined.
void XyToUv (double X, double Y, double& U, double& V)
{
	const double d = -2.0 * X + 12.0 * Y + 3.0;
	U = 4.0 * X / d;
	V = 6.0 * Y / d;
}

void UvToXy (double U, double V, double& X, double& Y)
{
	const double d = 2.0 * U - 8.0 * V + 4.0;
	X = 3.0 * U / d;
	Y = 2.0 * V / d;
}

// Display / working white (Rec.709 D65).
constexpr double kD65X = 0.3127;
constexpr double kD65Y = 0.3290;

// Scene illuminant chromaticity for a white-balance setting.
//
// The temperature curve is the Planckian locus shifted in CIE 1960 uv by the constant offset
// that takes Planck(6500 K) onto D65 (D65 sits ~+0.003 Duv above the locus). That makes
// 6500 K / tint 0 exactly the identity, keeps the curve continuous (switching to the CIE
// daylight locus at 4000 K, as some engines do, makes the tint jump), and tracks the daylight
// locus closely between 4000 and 10000 K. Tint then displaces the point along the local locus
// normal.
void IlluminantXy (double Kelvin, double Tint, double& X, double& Y)
{
	double px, py, pu, pv, ru, rv, du65, dv65;
	PlanckianLocusXy(Kelvin, px, py);
	XyToUv(px, py, pu, pv);
	PlanckianLocusXy(kReferenceWhiteKelvin, X, Y);
	XyToUv(X, Y, ru, rv);
	XyToUv(kD65X, kD65Y, du65, dv65);
	double u = pu + (du65 - ru);
	double v = pv + (dv65 - rv);

	if (Tint != 0.0)
	{
		const double t0 = std::clamp(Kelvin * 0.99, 1667.0, 25000.0);
		const double t1 = std::clamp(Kelvin * 1.01, 1667.0, 25000.0);
		double x0, y0, x1, y1, u0, v0, u1, v1;
		PlanckianLocusXy(t0, x0, y0);
		PlanckianLocusXy(t1, x1, y1);
		XyToUv(x0, y0, u0, v0);
		XyToUv(x1, y1, u1, v1);
		const double tu = u1 - u0;
		const double tv = v1 - v0;
		const double length = std::sqrt(tu * tu + tv * tv);
		if (length > 0.0)
		{
			// Normal toward +Duv (above the locus = greenish). +Tint assumes a greener illuminant,
			// so neutralising it moves the image toward magenta (camera / Lightroom convention).
			double nu = -tv / length;
			double nv = tu / length;
			if (nv < 0.0)
			{
				nu = -nu;
				nv = -nv;
			}
			u += nu * Tint * kTintDuvScale;
			v += nv * Tint * kTintDuvScale;
		}
	}
	UvToXy(u, v, X, Y);
}

// Rec.709 -> Rec.709 matrix: von Kries adaptation in Bradford LMS from the scene illuminant
// (Kelvin, Tint) to D65, then saturation around Rec.709 luminance. A light of the selected
// temperature renders as display white; 6500 K / tint 0 is the identity.
SMat3 ComputeColorMatrix (double Kelvin, double Tint, double Saturation)
{
	double sx, sy;
	IlluminantXy(Kelvin, Tint, sx, sy);
	const double dx = kD65X;
	const double dy = kD65Y;

	const double sourceXyz[3] = { sx / sy, 1.0, (1.0 - sx - sy) / sy };
	const double targetXyz[3] = { dx / dy, 1.0, (1.0 - dx - dy) / dy };
	double sourceLms[3], targetLms[3];
	Transform(kBradford, sourceXyz, sourceLms);
	Transform(kBradford, targetXyz, targetLms);

	const SMat3 gain = Diagonal(targetLms[0] / sourceLms[0], targetLms[1] / sourceLms[1], targetLms[2] / sourceLms[2]);
	const SMat3 adaptation = Multiply(Inverse(kBradford), Multiply(gain, kBradford));
	const SMat3 whiteBalance = Multiply(Inverse(kRec709ToXyz), Multiply(adaptation, kRec709ToXyz));

	// Saturation: lerp between luminance (Rec.709 Y row) and the colour, as one matrix.
	const double w[3] = { kRec709ToXyz.M[1][0], kRec709ToXyz.M[1][1], kRec709ToXyz.M[1][2] };
	SMat3 saturation;
	for (int r = 0; r < 3; ++r)
	{
		for (int c = 0; c < 3; ++c)
		{
			saturation.M[r][c] = (1.0 - Saturation) * w[c] + (r == c ? Saturation : 0.0);
		}
	}
	return Multiply(saturation, whiteBalance);
}

bool IsNearIdentity (const SMat3& A)
{
	for (int r = 0; r < 3; ++r)
	{
		for (int c = 0; c < 3; ++c)
		{
			if (std::abs(A.M[r][c] - (r == c ? 1.0 : 0.0)) > 1e-6)
			{
				return false;
			}
		}
	}
	return true;
}

gpu::float4 MatrixRow (const SMat3& A, int Row)
{
	return gpu::float4(static_cast<float>(A.M[Row][0]), static_cast<float>(A.M[Row][1]), static_cast<float>(A.M[Row][2]), 0.0f);
}


// ---------------------------------------------------------------------------------------------
// Output encodings by render target format.
// ---------------------------------------------------------------------------------------------

struct SOutputEncoding
{
	float QuantizationLevels = 255.0f;
	bool bSrgbTarget = false;   // hardware applies the OETF
	bool bScRgb = false;        // FP16 linear (no OETF, no dither)
};

SOutputEncoding ClassifyOutputFormat (DXGI_FORMAT Format)
{
	SOutputEncoding encoding;
	switch (Format)
	{
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			encoding.bSrgbTarget = true;
			break;
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			encoding.QuantizationLevels = 1023.0f;
			break;
		case DXGI_FORMAT_R16G16B16A16_UNORM:
			encoding.QuantizationLevels = 65535.0f;
			break;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			encoding.bScRgb = true;
			break;
		default:
			break;
	}
	return encoding;
}


// ---------------------------------------------------------------------------------------------
// Settings as the GPU sees them (every value clamped / repaired, user settings untouched).
// ---------------------------------------------------------------------------------------------

struct SResolvedSettings
{
	SPostProcessSettings S;
	float HistogramMinLog2 = 0.0f;
	float HistogramRange = 1.0f;
};

SResolvedSettings Resolve (const SPostProcessSettings& In)
{
	const SPostProcessSettings defaults;
	SResolvedSettings out;
	SPostProcessSettings& s = out.S;
	s = In;

	s.ExposureCompensationEV = SanitizeFloat(In.ExposureCompensationEV, 0.0f, -16.0f, 16.0f);
	s.ManualExposureEV = SanitizeFloat(In.ManualExposureEV, defaults.ManualExposureEV, -24.0f, 32.0f);
	s.MinEV = SanitizeFloat(In.MinEV, defaults.MinEV, -24.0f, 32.0f);
	s.MaxEV = SanitizeFloat(In.MaxEV, defaults.MaxEV, -24.0f, 32.0f);
	if (s.MaxEV < s.MinEV + 0.25f)
	{
		s.MaxEV = s.MinEV + 0.25f;
	}
	s.AdaptationSpeedUp = SanitizeFloat(In.AdaptationSpeedUp, defaults.AdaptationSpeedUp, 0.01f, 100.0f);
	s.AdaptationSpeedDown = SanitizeFloat(In.AdaptationSpeedDown, defaults.AdaptationSpeedDown, 0.01f, 100.0f);
	s.AdaptationTransitionEV = SanitizeFloat(In.AdaptationTransitionEV, defaults.AdaptationTransitionEV, 0.01f, 8.0f);
	s.HistogramLowPercentile = SanitizeFloat(In.HistogramLowPercentile, defaults.HistogramLowPercentile, 0.0f, 0.99f);
	s.HistogramHighPercentile = SanitizeFloat(In.HistogramHighPercentile, defaults.HistogramHighPercentile, 0.0f, 1.0f);
	s.HistogramHighPercentile = (std::max)(s.HistogramHighPercentile, s.HistogramLowPercentile + 0.005f);
	s.MeteringCenterWeight = SanitizeFloat(In.MeteringCenterWeight, defaults.MeteringCenterWeight, 0.0f, 1.0f);
	s.MeteringSpotSize = SanitizeFloat(In.MeteringSpotSize, defaults.MeteringSpotSize, 0.05f, 4.0f);

	s.BloomIntensity = SanitizeFloat(In.BloomIntensity, defaults.BloomIntensity, 0.0f, 1.0f);
	s.BloomFalloff = SanitizeFloat(In.BloomFalloff, defaults.BloomFalloff, 0.1f, 1.0f);
	s.BloomRadius = SanitizeFloat(In.BloomRadius, defaults.BloomRadius, 0.1f, 4.0f);
	s.BloomMips = std::clamp(In.BloomMips, 1u, kMaxBloomLevels);

	if (static_cast<uint32>(s.ToneMapper) >= static_cast<uint32>(EToneMapper::Count))
	{
		s.ToneMapper = EToneMapper::AgX;
	}
	if (static_cast<uint32>(s.AgXLook) >= static_cast<uint32>(EAgXLook::Count))
	{
		s.AgXLook = EAgXLook::Base;
	}
	s.Saturation = SanitizeFloat(In.Saturation, 1.0f, 0.0f, 4.0f);
	s.Contrast = SanitizeFloat(In.Contrast, 1.0f, 0.05f, 4.0f);
	s.WhiteBalanceTemperature = SanitizeFloat(In.WhiteBalanceTemperature, 6500.0f, 1667.0f, 25000.0f);
	s.WhiteBalanceTint = SanitizeFloat(In.WhiteBalanceTint, 0.0f, -1.0f, 1.0f);
	s.VignetteIntensity = SanitizeFloat(In.VignetteIntensity, 0.0f, 0.0f, 4.0f);
	s.ReinhardWhitePoint = SanitizeFloat(In.ReinhardWhitePoint, 0.0f, 0.0f, 1e4f);
	s.HdrPaperWhiteNits = SanitizeFloat(In.HdrPaperWhiteNits, defaults.HdrPaperWhiteNits, 10.0f, 10000.0f);

	// Histogram over log2 luminance: EV100 = log2(L) + 3.
	out.HistogramMinLog2 = s.MinEV - gpu::ExposureLog2CalibrationOffset - kHistogramMarginEV;
	out.HistogramRange = (s.MaxEV - s.MinEV) + 2.0f * kHistogramMarginEV;
	return out;
}

rhi::STextureDesc MakeBloomDesc (uint32 Width, uint32 Height, const wchar_t* Name)
{
	rhi::STextureDesc desc;
	desc.Width = Width;
	desc.Height = Height;
	desc.Format = kBloomFormat;
	desc.bSrv = true;
	desc.bUav = true;
	desc.InitialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	desc.Name = Name;
	return desc;
}
}


struct CPostProcess::SImpl
{
	struct SFinalPipeline
	{
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
		rhi::HPipeline Pipeline = rhi::InvalidPipelineHandle;
	};

	struct SReadbackSlot
	{
		rhi::SGpuBuffer Buffer;
		uint64 Fence = 0u;      // submission fence of the frame that recorded the copy
		uint64 Serial = 0u;     // Execute counter of that frame
		bool bPending = false;  // copy recorded, not consumed yet
	};

	rhi::CPipelineLibrary* Pipelines = nullptr;

	rhi::HPipeline ExposureHistogram = rhi::InvalidPipelineHandle;
	rhi::HPipeline ExposureAdapt = rhi::InvalidPipelineHandle;
	rhi::HPipeline BloomPrefilter = rhi::InvalidPipelineHandle;
	rhi::HPipeline BloomDownsample = rhi::InvalidPipelineHandle;
	rhi::HPipeline BloomUpsample = rhi::InvalidPipelineHandle;
	std::vector<SFinalPipeline> FinalPipelines;

	rhi::SGpuBuffer HistogramBuffer;
	rhi::SGpuBuffer ExposureBuffer;
	SReadbackSlot Readback[kReadbackSlots];
	uint64 ExecuteSerial = 0u;
	bool bHistogramNeedsClear = true;
	uint32 ExposureSettleFramesLeft = 0u;   // see kExposureSettleFrames
	std::chrono::steady_clock::time_point LastExecuteTime = {};
	bool bHasLastExecuteTime = false;

	rhi::SGpuTexture BloomDown[kMaxBloomLevels];
	rhi::SGpuTexture BloomUp[kMaxBloomLevels];
	uint32 BloomLevelCount = 0u;
	uint32 ChainWidth = 0u;
	uint32 ChainHeight = 0u;

	// CPU mirror of the newest completed readback (two frames old).
	gpu::SExposureState LastState = {};
	uint32 LastHistogram[gpu::ExposureHistogramBins] = {};
	float LastExposure = 1.0f;
	bool bHasReadback = false;
	float UiHistogram[gpu::ExposureHistogramBins] = {};

	bool HasPipeline (rhi::HPipeline Handle) const
	{
		return Pipelines && Pipelines->Get(Handle) != nullptr;
	}

	void DestroyBloomChain (rhi::CDevice& Device)
	{
		for (uint32 i = 0; i < kMaxBloomLevels; ++i)
		{
			if (BloomDown[i].IsValid())
			{
				Device.Destroy(BloomDown[i]);
			}
			if (BloomUp[i].IsValid())
			{
				Device.Destroy(BloomUp[i]);
			}
		}
		BloomLevelCount = 0u;
		ChainWidth = 0u;
		ChainHeight = 0u;
	}

	// Level i is ceil(size / 2^(i+1)); levels stop once a side would drop below 2 texels. The
	// chain is always allocated to the maximum depth so BloomMips can change without realloc.
	void CreateBloomChain (rhi::CDevice& Device, uint32 Width, uint32 Height)
	{
		DestroyBloomChain(Device);
		if (Width == 0u || Height == 0u)
		{
			return;
		}

		uint32 w = Width;
		uint32 h = Height;
		for (uint32 i = 0; i < kMaxBloomLevels; ++i)
		{
			w = (std::max)(1u, (w + 1u) / 2u);
			h = (std::max)(1u, (h + 1u) / 2u);
			if (i > 0u && (w < 2u || h < 2u))
			{
				break;
			}
			const std::wstring name = L"PostProcess.BloomDown" + std::to_wstring(i);
			BloomDown[i] = Device.CreateTexture(MakeBloomDesc(w, h, name.c_str()));
			BloomLevelCount = i + 1u;
		}
		// Up[i] = Down[i] + upsampled coarser levels; the coarsest level is its own Up.
		for (uint32 i = 0; i + 1u < BloomLevelCount; ++i)
		{
			const std::wstring name = L"PostProcess.BloomUp" + std::to_wstring(i);
			BloomUp[i] = Device.CreateTexture(MakeBloomDesc(BloomDown[i].Width, BloomDown[i].Height, name.c_str()));
		}
		ChainWidth = Width;
		ChainHeight = Height;
	}

	rhi::HPipeline GetFinalPipeline (DXGI_FORMAT Format)
	{
		for (const SFinalPipeline& entry : FinalPipelines)
		{
			if (entry.Format == Format)
			{
				return entry.Pipeline;
			}
		}
		rhi::SGraphicsPipelineDesc desc;
		desc.VertexShader = MakeShaderDesc("PostProcess/FinalPass.hlsl", "VSMain", "vs_6_6");
		desc.PixelShader = MakeShaderDesc("PostProcess/FinalPass.hlsl", "PSMain", "ps_6_6");
		desc.RenderTargetFormats[0] = Format;
		desc.RenderTargetCount = 1u;
		desc.CullMode = D3D12_CULL_MODE_NONE;
		char name[64];
		std::snprintf(name, sizeof(name), "PostProcess_Final_fmt%u", static_cast<uint32>(Format));
		SFinalPipeline entry;
		entry.Format = Format;
		entry.Pipeline = Pipelines->CreateGraphics(name, desc);
		FinalPipelines.push_back(entry);
		return entry.Pipeline;
	}

	// A slot is safe to read / reuse once the GPU has finished the frame that wrote it: at least
	// MaxFramesInFlight frames later both by submission fence (what the renderer's fence wait
	// guarantees) and by our own frame count (robust if fence values advance unevenly).
	bool IsSlotComplete (const SReadbackSlot& Slot, uint64 Fence) const
	{
		return Slot.Fence + rhi::MaxFramesInFlight <= Fence && Slot.Serial + rhi::MaxFramesInFlight <= ExecuteSerial;
	}

	void ConsumeReadback (const rhi::CDevice& Device)
	{
		const uint64 fence = Device.GetSubmissionFenceValue();
		SReadbackSlot* newest = nullptr;
		for (SReadbackSlot& slot : Readback)
		{
			if (slot.bPending && slot.Buffer.Mapped && IsSlotComplete(slot, fence) && (!newest || slot.Serial > newest->Serial))
			{
				newest = &slot;
			}
		}
		if (!newest)
		{
			return;
		}

		gpu::SExposureState state;
		std::memcpy(&state, newest->Buffer.Mapped, sizeof(state));
		std::memcpy(LastHistogram, newest->Buffer.Mapped + gpu::ExposureHistogramSnapshotOffset, sizeof(LastHistogram));
		if (state.Valid == gpu::ExposureStateValidMagic && std::isfinite(state.Exposure) && state.Exposure > 0.0f)
		{
			LastState = state;
			LastExposure = state.Exposure;
			bHasReadback = true;
		}
		// Everything at or before the newest completed copy is now stale.
		for (SReadbackSlot& slot : Readback)
		{
			if (slot.bPending && slot.Serial <= newest->Serial && IsSlotComplete(slot, fence))
			{
				slot.bPending = false;
			}
		}
	}

	SReadbackSlot* AcquireReadbackSlot (const rhi::CDevice& Device)
	{
		const uint64 fence = Device.GetSubmissionFenceValue();
		for (SReadbackSlot& slot : Readback)
		{
			if (slot.Buffer.IsValid() && (!slot.bPending || IsSlotComplete(slot, fence) || slot.Fence == fence))
			{
				return &slot;
			}
		}
		return nullptr; // every slot still in flight: skip this frame's copy
	}
};


void CPostProcess::Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines)
{
	if (Impl)
	{
		Shutdown(Device);
	}
	Impl = new SImpl();
	SImpl& impl = *Impl;
	impl.Pipelines = &Pipelines;

	impl.ExposureHistogram = Pipelines.CreateCompute("PostProcess_ExposureHistogram",
		MakeShaderDesc("PostProcess/ExposureHistogram.hlsl", "main", "cs_6_6"));
	impl.ExposureAdapt = Pipelines.CreateCompute("PostProcess_ExposureAdapt",
		MakeShaderDesc("PostProcess/ExposureAdapt.hlsl", "main", "cs_6_6"));
	impl.BloomPrefilter = Pipelines.CreateCompute("PostProcess_BloomPrefilter",
		MakeShaderDesc("PostProcess/BloomPrefilter.hlsl", "main", "cs_6_6"));
	impl.BloomDownsample = Pipelines.CreateCompute("PostProcess_BloomDownsample",
		MakeShaderDesc("PostProcess/BloomDownsample.hlsl", "main", "cs_6_6"));
	impl.BloomUpsample = Pipelines.CreateCompute("PostProcess_BloomUpsample",
		MakeShaderDesc("PostProcess/BloomUpsample.hlsl", "main", "cs_6_6"));
	impl.GetFinalPipeline(kDefaultOutputFormat);

	rhi::SBufferDesc histogramDesc;
	histogramDesc.Size = gpu::ExposureHistogramBins * sizeof(uint32);
	histogramDesc.UavKind = rhi::EBufferViewKind::Raw;
	histogramDesc.Name = L"PostProcess.ExposureHistogram";
	impl.HistogramBuffer = Device.CreateBuffer(histogramDesc);

	rhi::SBufferDesc exposureDesc;
	exposureDesc.Size = gpu::ExposureBufferBytes;
	exposureDesc.SrvKind = rhi::EBufferViewKind::Raw;
	exposureDesc.UavKind = rhi::EBufferViewKind::Raw;
	exposureDesc.Name = L"PostProcess.Exposure";
	impl.ExposureBuffer = Device.CreateBuffer(exposureDesc);

	for (uint32 i = 0; i < kReadbackSlots; ++i)
	{
		rhi::SBufferDesc readbackDesc;
		readbackDesc.Size = gpu::ExposureBufferBytes;
		readbackDesc.HeapType = D3D12_HEAP_TYPE_READBACK;
		readbackDesc.Name = L"PostProcess.ExposureReadback";
		impl.Readback[i].Buffer = Device.CreateBuffer(readbackDesc);
	}
	impl.bHistogramNeedsClear = true;
}

void CPostProcess::Shutdown (rhi::CDevice& Device)
{
	if (!Impl)
	{
		return;
	}
	SImpl& impl = *Impl;
	impl.DestroyBloomChain(Device);
	if (impl.HistogramBuffer.IsValid())
	{
		Device.Destroy(impl.HistogramBuffer);
	}
	if (impl.ExposureBuffer.IsValid())
	{
		Device.Destroy(impl.ExposureBuffer);
	}
	for (SImpl::SReadbackSlot& slot : impl.Readback)
	{
		if (slot.Buffer.IsValid())
		{
			Device.Destroy(slot.Buffer);
		}
	}
	delete Impl;
	Impl = nullptr;
}

void CPostProcess::Resize (rhi::CDevice& Device, uint32 DisplayWidth, uint32 DisplayHeight)
{
	if (!Impl)
	{
		return;
	}
	if (DisplayWidth != Impl->ChainWidth || DisplayHeight != Impl->ChainHeight || Impl->BloomLevelCount == 0u)
	{
		Impl->CreateBloomChain(Device, DisplayWidth, DisplayHeight);
	}
}

float CPostProcess::GetLastExposure () const
{
	return Impl ? Impl->LastExposure : 1.0f;
}

uint32 CPostProcess::GetExposureBufferSrv () const
{
	return Impl ? Impl->ExposureBuffer.Srv : rhi::InvalidDescriptorIndex;
}

void CPostProcess::Execute (CRenderContext& Context, const SPostProcessInputs& Inputs)
{
	if (!Impl || !Context.CommandList || !Context.Device || !Context.Pipelines)
	{
		return;
	}
	SImpl& impl = *Impl;
	rhi::CDevice& device = *Context.Device;
	ID3D12GraphicsCommandList4* commandList = Context.CommandList;

	const SResolvedSettings resolved = Resolve(Settings);
	const SPostProcessSettings& s = resolved.S;

	rhi::SGpuTexture* hdr = (Inputs.HdrColor && Inputs.HdrColor->IsValid() && Inputs.HdrColor->Srv != rhi::InvalidDescriptorIndex)
		? Inputs.HdrColor : nullptr;
	rhi::SGpuTexture* debug = (Inputs.DebugTexture && Inputs.DebugTexture->IsValid() && Inputs.DebugTexture->Srv != rhi::InvalidDescriptorIndex)
		? Inputs.DebugTexture : nullptr;

	uint32 outputWidth = Inputs.OutputWidth;
	uint32 outputHeight = Inputs.OutputHeight;
	if ((outputWidth == 0u || outputHeight == 0u) && hdr)
	{
		outputWidth = hdr->Width;
		outputHeight = hdr->Height;
	}
	if (outputWidth == 0u || outputHeight == 0u || Inputs.OutputRtv.ptr == 0u)
	{
		return;
	}

	impl.ConsumeReadback(device);
	Context.BindCompute();

	// Robust to a missed Resize: the pyramid always follows the HDR input.
	if (hdr && (hdr->Width != impl.ChainWidth || hdr->Height != impl.ChainHeight || impl.BloomLevelCount == 0u))
	{
		impl.CreateBloomChain(device, hdr->Width, hdr->Height);
	}

	const float manualExposure = std::exp2(s.ExposureCompensationEV - s.ManualExposureEV);
	const float fallbackExposure = s.bAutoExposure ? impl.LastExposure : manualExposure;
	const bool bExposureBuffer = impl.ExposureBuffer.IsValid() && impl.HistogramBuffer.IsValid();

	// Wall-clock frame time, only used by the shader while g_Frame.DeltaTime is not provided.
	const auto now = std::chrono::steady_clock::now();
	const float cpuDeltaTime = impl.bHasLastExecuteTime
		? std::clamp(std::chrono::duration<float>(now - impl.LastExecuteTime).count(), 0.0f, 1.0f) : 0.0f;
	impl.LastExecuteTime = now;
	impl.bHasLastExecuteTime = true;

	// ---- 1. Auto exposure ------------------------------------------------------------------
	if (bExposureBuffer && impl.HasPipeline(impl.ExposureAdapt))
	{
		FRT_GPU_SCOPE(commandList, "PostProcess_Exposure");

		gpu::SExposurePassConstants constants = {};
		constants.InputTexture = hdr ? hdr->Srv : rhi::InvalidDescriptorIndex;
		constants.HistogramBuffer = impl.HistogramBuffer.Uav;
		constants.ExposureBuffer = impl.ExposureBuffer.Uav;
		constants.InputSize = gpu::uint2(hdr ? hdr->Width : 0u, hdr ? hdr->Height : 0u);
		constants.InvInputSize = gpu::float2(hdr ? 1.0f / static_cast<float>(hdr->Width) : 0.0f, hdr ? 1.0f / static_cast<float>(hdr->Height) : 0.0f);
		constants.MinLog2Luminance = resolved.HistogramMinLog2;
		constants.Log2LuminanceRange = resolved.HistogramRange;
		constants.InvLog2LuminanceRange = 1.0f / resolved.HistogramRange;
		constants.CenterWeight = s.MeteringCenterWeight;
		constants.LowPercentile = s.HistogramLowPercentile;
		constants.HighPercentile = s.HistogramHighPercentile;
		constants.MinEV = s.MinEV;
		constants.MaxEV = s.MaxEV;
		constants.SpeedUp = s.AdaptationSpeedUp;
		constants.SpeedDown = s.AdaptationSpeedDown;
		constants.TransitionEV = s.AdaptationTransitionEV;
		constants.CompensationEV = s.ExposureCompensationEV;
		constants.ManualEV = s.ManualExposureEV;
		constants.HalfHeightInv = hdr ? 2.0f / static_cast<float>(hdr->Height) : 0.0f;
		constants.SpotInvTwoSigmaSq = 1.0f / (2.0f * s.MeteringSpotSize * s.MeteringSpotSize);
		constants.FallbackDeltaTime = cpuDeltaTime;

		Context.Barriers.Transition(impl.HistogramBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Context.Barriers.Transition(impl.ExposureBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		if (impl.bHistogramNeedsClear)
		{
			// Never trust the initial contents; later frames are cleared by ExposureAdapt.
			gpu::SExposurePassConstants clear = constants;
			clear.Flags = gpu::ExposureFlag_ClearOnly;
			Context.PushPassConstants(clear);
			Context.Dispatch(impl.ExposureAdapt, 1u, 1u, 1u);
			Context.Barriers.Uav(impl.HistogramBuffer);
			impl.bHistogramNeedsClear = false;
		}

		const bool bMeter = s.bAutoExposure && hdr && impl.HasPipeline(impl.ExposureHistogram);
		if (bMeter)
		{
			// The histogram stays in UAV state across Execute calls, so the Transition above is a
			// no-op: order this pass's atomics after the reset written by a previous ExposureAdapt
			// recorded into the same command list (e.g. two Execute calls in one frame).
			Context.Barriers.Uav(impl.HistogramBuffer);
			Context.Barriers.Transition(*hdr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			constants.Flags = gpu::ExposureFlag_Auto;
			Context.PushPassConstants(constants);
			Context.Dispatch(impl.ExposureHistogram,
				rhi::DivideRoundUp(hdr->Width, kHistogramTile), rhi::DivideRoundUp(hdr->Height, kHistogramTile), 1u);
			Context.Barriers.Uav(impl.HistogramBuffer);
		}

		if (Inputs.bResetExposure)
		{
			impl.ExposureSettleFramesLeft = kExposureSettleFrames;
		}
		const bool bSnapExposure = impl.ExposureSettleFramesLeft > 0u;
		impl.ExposureSettleFramesLeft -= bSnapExposure ? 1u : 0u;
		constants.Flags = (s.bAutoExposure ? gpu::ExposureFlag_Auto : 0u) | (bSnapExposure ? gpu::ExposureFlag_Reset : 0u);
		Context.PushPassConstants(constants);
		Context.Dispatch(impl.ExposureAdapt, 1u, 1u, 1u);

		// Readable by every later pass this frame and next frame, and by the readback copy.
		Context.Barriers.Transition(impl.ExposureBuffer, kAnyShaderRead | D3D12_RESOURCE_STATE_COPY_SOURCE);
		Context.FlushBarriers();

		if (SImpl::SReadbackSlot* slot = impl.AcquireReadbackSlot(device))
		{
			commandList->CopyBufferRegion(slot->Buffer.Resource.Get(), 0u, impl.ExposureBuffer.Resource.Get(), 0u, gpu::ExposureBufferBytes);
			slot->Fence = device.GetSubmissionFenceValue();
			slot->Serial = impl.ExecuteSerial;
			slot->bPending = true;
		}
	}

	// ---- 2. Bloom ---------------------------------------------------------------------------
	rhi::SGpuTexture* bloomResult = nullptr;
	uint32 bloomLevels = 0u;
	const bool bBloom = hdr && s.bBloom && s.BloomIntensity > 0.0f && impl.BloomLevelCount > 0u
		&& impl.HasPipeline(impl.BloomPrefilter) && impl.HasPipeline(impl.BloomDownsample) && impl.HasPipeline(impl.BloomUpsample);
	if (bBloom)
	{
		FRT_GPU_SCOPE(commandList, "PostProcess_Bloom");
		bloomLevels = (std::min)(s.BloomMips, impl.BloomLevelCount);

		gpu::SBloomPassConstants constants = {};
		constants.ExposureBuffer = bExposureBuffer ? impl.ExposureBuffer.Srv : rhi::InvalidDescriptorIndex;
		constants.FallbackExposure = fallbackExposure;
		constants.MaxExposedValue = kMaxExposedBloomInput;
		constants.FilterRadius = s.BloomRadius;
		constants.CurrentTexture = rhi::InvalidDescriptorIndex;

		auto setLevel = [&constants] (const rhi::SGpuTexture& Source, const rhi::SGpuTexture& Output)
		{
			constants.SourceTexture = Source.Srv;
			constants.OutputTexture = Output.Uav;
			constants.SourceSize = gpu::uint2(Source.Width, Source.Height);
			constants.OutputSize = gpu::uint2(Output.Width, Output.Height);
			constants.SourceTexelSize = gpu::float2(1.0f / static_cast<float>(Source.Width), 1.0f / static_cast<float>(Source.Height));
			constants.InvOutputSize = gpu::float2(1.0f / static_cast<float>(Output.Width), 1.0f / static_cast<float>(Output.Height));
		};

		// Level 0: exposed, sanitised, Karis-weighted 13-tap downsample of the HDR image.
		Context.Barriers.Transition(*hdr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Context.Barriers.Transition(impl.BloomDown[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		setLevel(*hdr, impl.BloomDown[0]);
		Context.PushPassConstants(constants);
		Context.Dispatch2D(impl.BloomPrefilter, impl.BloomDown[0].Width, impl.BloomDown[0].Height);

		// Downsample chain.
		for (uint32 i = 1u; i < bloomLevels; ++i)
		{
			Context.Barriers.Transition(impl.BloomDown[i - 1u], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Context.Barriers.Transition(impl.BloomDown[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			setLevel(impl.BloomDown[i - 1u], impl.BloomDown[i]);
			Context.PushPassConstants(constants);
			Context.Dispatch2D(impl.BloomDownsample, impl.BloomDown[i].Width, impl.BloomDown[i].Height);
		}

		// Upsample chain, starting from the coarsest Down: Up[i] is the weighted running mean of
		// Down[i] and the tent-upsampled levels below it,
		//   Up[i] = lerp(tent(Up[i + 1]), Down[i], w_i / (w_i + ... + w_{N-1})),  w_i = Falloff^i
		// (BloomUpsample.hlsl). Same result as the weighted sum of N levels normalised by the
		// weight total, without the RGBA16F overflow of the sum. Up[0] holds the image energy.
		float levelWeights[kMaxBloomLevels] = {};
		for (uint32 i = 0u; i < bloomLevels; ++i)
		{
			levelWeights[i] = std::pow(s.BloomFalloff, static_cast<float>(i));
		}
		rhi::SGpuTexture* lower = &impl.BloomDown[bloomLevels - 1u];
		float tailWeight = levelWeights[bloomLevels - 1u];
		for (uint32 level = bloomLevels - 1u; level-- > 0u;)
		{
			rhi::SGpuTexture& current = impl.BloomDown[level];
			rhi::SGpuTexture& output = impl.BloomUp[level];
			Context.Barriers.Transition(*lower, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Context.Barriers.Transition(current, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Context.Barriers.Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			setLevel(*lower, output);
			constants.CurrentTexture = current.Srv;
			tailWeight += levelWeights[level];
			constants.CurrentWeight = levelWeights[level] / tailWeight;
			Context.PushPassConstants(constants);
			Context.Dispatch2D(impl.BloomUpsample, output.Width, output.Height);
			lower = &output;
		}
		bloomResult = lower;
	}

	// ---- 3. Display transform (full-screen triangle into the back buffer) -------------------
	{
		FRT_GPU_SCOPE(commandList, "PostProcess_Final");

		const DXGI_FORMAT outputFormat = Inputs.OutputFormat != DXGI_FORMAT_UNKNOWN ? Inputs.OutputFormat : kDefaultOutputFormat;
		const rhi::HPipeline pipeline = impl.GetFinalPipeline(outputFormat);
		ID3D12PipelineState* pso = Context.Pipelines->Get(pipeline);

		Context.BindGraphics();
		if (hdr)
		{
			Context.Barriers.Transition(*hdr, kAnyShaderRead);
		}
		if (bloomResult)
		{
			Context.Barriers.Transition(*bloomResult, kAnyShaderRead);
		}
		if (debug)
		{
			Context.Barriers.Transition(*debug, kAnyShaderRead);
		}
		Context.FlushBarriers();

		commandList->OMSetRenderTargets(1u, &Inputs.OutputRtv, FALSE, nullptr);
		const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(outputWidth), static_cast<float>(outputHeight), 0.0f, 1.0f };
		const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(outputWidth), static_cast<LONG>(outputHeight) };
		commandList->RSSetViewports(1u, &viewport);
		commandList->RSSetScissorRects(1u, &scissor);

		if (!pso || (!hdr && !debug))
		{
			// Nothing to show (or the pass failed to compile): a defined black frame.
			const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			commandList->ClearRenderTargetView(Inputs.OutputRtv, black, 0u, nullptr);
		}
		else
		{
			const SOutputEncoding encoding = ClassifyOutputFormat(outputFormat);
			const SMat3 colorMatrix = ComputeColorMatrix(s.WhiteBalanceTemperature, s.WhiteBalanceTint, s.Saturation);

			gpu::SPostFinalConstants constants = {};
			constants.ColorMatrixR = MatrixRow(colorMatrix, 0);
			constants.ColorMatrixG = MatrixRow(colorMatrix, 1);
			constants.ColorMatrixB = MatrixRow(colorMatrix, 2);
			constants.HdrTexture = hdr ? hdr->Srv : rhi::InvalidDescriptorIndex;
			constants.BloomTexture = bloomResult ? bloomResult->Srv : rhi::InvalidDescriptorIndex;
			constants.ExposureBuffer = bExposureBuffer ? impl.ExposureBuffer.Srv : rhi::InvalidDescriptorIndex;
			constants.DebugTexture = debug ? debug->Srv : rhi::InvalidDescriptorIndex;
			constants.OutputSize = gpu::float2(static_cast<float>(outputWidth), static_cast<float>(outputHeight));
			constants.InvOutputSize = gpu::float2(1.0f / static_cast<float>(outputWidth), 1.0f / static_cast<float>(outputHeight));
			if (bloomResult)
			{
				constants.BloomTapOffset = gpu::float2(s.BloomRadius / static_cast<float>(bloomResult->Width), s.BloomRadius / static_cast<float>(bloomResult->Height));
			}
			constants.BloomIntensity = s.BloomIntensity;
			constants.Contrast = s.Contrast;
			constants.VignetteStrength = s.VignetteIntensity;
			constants.AspectRatio = static_cast<float>(outputWidth) / static_cast<float>(outputHeight);
			constants.ReinhardWhitePoint = s.ReinhardWhitePoint;
			constants.ToneMapper = static_cast<uint32>(s.ToneMapper);
			constants.AgXLook = static_cast<uint32>(s.AgXLook);
			constants.QuantizationLevels = encoding.QuantizationLevels;
			constants.FallbackExposure = fallbackExposure;
			constants.ScRgbScale = s.HdrPaperWhiteNits / 80.0f;

			uint32 flags = 0u;
			flags |= bloomResult ? gpu::FinalFlag_Bloom : 0u;
			flags |= s.bDither ? gpu::FinalFlag_Dither : 0u;
			flags |= debug ? gpu::FinalFlag_Debug : 0u;
			flags |= (debug && Inputs.bDebugIsLinear) ? gpu::FinalFlag_DebugLinear : 0u;
			flags |= (debug && Inputs.bDebugIsLinear && s.bDebugToneMap) ? gpu::FinalFlag_DebugToneMap : 0u;
			flags |= hdr ? gpu::FinalFlag_HasHdr : 0u;
			flags |= (hdr && hdr->Width == outputWidth && hdr->Height == outputHeight) ? gpu::FinalFlag_HdrSameSize : 0u;
			flags |= encoding.bSrgbTarget ? gpu::FinalFlag_SrgbRenderTarget : 0u;
			flags |= encoding.bScRgb ? gpu::FinalFlag_ScRgbOutput : 0u;
			flags |= (!IsNearIdentity(colorMatrix) || s.Contrast != 1.0f) ? gpu::FinalFlag_Grading : 0u;
			flags |= s.VignetteIntensity > 0.0f ? gpu::FinalFlag_Vignette : 0u;
			constants.Flags = flags;

			commandList->SetPipelineState(pso);
			Context.PushGraphicsPassConstants(constants);
			commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			commandList->DrawInstanced(3u, 1u, 0u, 0u);
		}

		// Documented exit states (the RTV stays bound for the caller's UI pass).
		if (hdr)
		{
			Context.Barriers.Transition(*hdr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
		if (debug && debug != hdr)
		{
			Context.Barriers.Transition(*debug, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
		Context.FlushBarriers();
	}

	++impl.ExecuteSerial;
}

void CPostProcess::DrawSettingsUi ()
{
	SPostProcessSettings& s = Settings;
	const SPostProcessSettings defaults;
	ImGui::PushID("PostProcess");

	ImGui::SeparatorText("Exposure");
	ImGui::Checkbox("Auto exposure", &s.bAutoExposure);
	ImGui::SliderFloat("Compensation (EV)", &s.ExposureCompensationEV, -8.0f, 8.0f, "%+.2f");
	if (!s.bAutoExposure)
	{
		ImGui::SliderFloat("Manual EV100", &s.ManualExposureEV, -8.0f, 24.0f, "%.2f");
		ImGui::SetItemTooltip("Exposure = 2^(compensation - EV). Sunny day ~15, interior ~7, night ~0.");
	}
	else
	{
		ImGui::DragFloatRange2("EV100 clamp", &s.MinEV, &s.MaxEV, 0.05f, -16.0f, 28.0f, "min %.1f", "max %.1f");
		ImGui::SliderFloat("Adapt to bright (EV/s)", &s.AdaptationSpeedUp, 0.05f, 50.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Adapt to dark (EV/s)", &s.AdaptationSpeedDown, 0.05f, 50.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Ease-in distance (EV)", &s.AdaptationTransitionEV, 0.05f, 4.0f, "%.2f");
		ImGui::SetItemTooltip("Farther than this the adaptation is linear (EV/s); closer it settles exponentially.");
		ImGui::DragFloatRange2("Metered percentiles", &s.HistogramLowPercentile, &s.HistogramHighPercentile, 0.002f, 0.0f, 1.0f, "low %.3f", "high %.3f");
		ImGui::SetItemTooltip("Average of log luminance between these fractions of the weighted histogram.");
		ImGui::SliderFloat("Centre weighting", &s.MeteringCenterWeight, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Centre spot size", &s.MeteringSpotSize, 0.1f, 2.0f, "%.2f");
	}

	if (Impl && Impl->bHasReadback)
	{
		const gpu::SExposureState& state = Impl->LastState;
		ImGui::Text("Exposure x%.4g   EV100 %.2f (target %.2f)", state.Exposure, state.AdaptedEV, state.TargetEV);
		if (s.bAutoExposure)
		{
			ImGui::Text("Metered mean log2 L %.2f", state.AverageLog2Luminance);

			uint32 peak = 1u;
			for (uint32 bin : Impl->LastHistogram)
			{
				peak = (std::max)(peak, bin);
			}
			for (uint32 i = 0; i < gpu::ExposureHistogramBins; ++i)
			{
				Impl->UiHistogram[i] = static_cast<float>(Impl->LastHistogram[i]) / static_cast<float>(peak);
			}
			const SResolvedSettings resolved = Resolve(s);
			char overlay[64];
			std::snprintf(overlay, sizeof(overlay), "EV100 %.1f .. %.1f",
				resolved.HistogramMinLog2 + gpu::ExposureLog2CalibrationOffset,
				resolved.HistogramMinLog2 + resolved.HistogramRange + gpu::ExposureLog2CalibrationOffset);
			ImGui::PlotHistogram("##ExposureHistogram", Impl->UiHistogram, static_cast<int>(gpu::ExposureHistogramBins), 0,
				overlay, 0.0f, 1.0f, ImVec2(0.0f, 64.0f));
		}
	}

	ImGui::SeparatorText("Bloom");
	ImGui::Checkbox("Bloom", &s.bBloom);
	ImGui::BeginDisabled(!s.bBloom);
	ImGui::SliderFloat("Intensity", &s.BloomIntensity, 0.0f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic);
	ImGui::SetItemTooltip("Fraction of the image energy redistributed into the bloom (energy conserving).");
	ImGui::SliderFloat("Falloff per level", &s.BloomFalloff, 0.1f, 1.0f, "%.2f");
	ImGui::SetItemTooltip("Energy ratio between successive pyramid levels (radius doubles per level). "
		"0.5: glare tail ~ r^-3 (camera / eye veiling glare). 1: equal energy per octave (wide haze).");
	ImGui::SliderFloat("Radius", &s.BloomRadius, 0.25f, 3.0f, "%.2f");
	int mips = static_cast<int>(s.BloomMips);
	if (ImGui::SliderInt("Pyramid levels", &mips, 1, static_cast<int>(kMaxBloomLevels)))
	{
		s.BloomMips = static_cast<uint32>(mips);
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText("Tone mapping");
	static const char* const kToneMappers[] = { "AgX", "ACES (Hill fit)", "Khronos PBR Neutral", "Reinhard (luminance)", "Linear (clip)" };
	int toneMapper = static_cast<int>(s.ToneMapper);
	if (ImGui::Combo("Tone mapper", &toneMapper, kToneMappers, static_cast<int>(std::size(kToneMappers))))
	{
		s.ToneMapper = static_cast<EToneMapper>(toneMapper);
	}
	if (s.ToneMapper == EToneMapper::AgX)
	{
		static const char* const kLooks[] = { "Base", "Punchy", "Golden" };
		int look = static_cast<int>(s.AgXLook);
		if (ImGui::Combo("AgX look", &look, kLooks, static_cast<int>(std::size(kLooks))))
		{
			s.AgXLook = static_cast<EAgXLook>(look);
		}
	}
	else if (s.ToneMapper == EToneMapper::Reinhard)
	{
		ImGui::SliderFloat("White point (0 = off)", &s.ReinhardWhitePoint, 0.0f, 64.0f, "%.2f");
	}
	ImGui::Checkbox("Dither (TPDF, 1 LSB)", &s.bDither);
	ImGui::Checkbox("Expose + tone map debug views", &s.bDebugToneMap);
	ImGui::SetItemTooltip("For radiance debug views (direct / indirect only, emission). Off: linear debug values are shown exactly.");
	ImGui::SliderFloat("HDR paper white (nits)", &s.HdrPaperWhiteNits, 80.0f, 500.0f, "%.0f");
	ImGui::SetItemTooltip("Only used for an FP16 (scRGB) swap chain.");

	ImGui::SeparatorText("Grading");
	ImGui::SliderFloat("White balance (K)", &s.WhiteBalanceTemperature, 2000.0f, 12000.0f, "%.0f K");
	ImGui::SetItemTooltip("Scene illuminant to neutralise: lights of this temperature render white (lower = cooler image).");
	ImGui::SliderFloat("Tint", &s.WhiteBalanceTint, -1.0f, 1.0f, "%+.2f");
	ImGui::SetItemTooltip("+ = magenta (removes a green cast), - = green.");
	ImGui::SliderFloat("Saturation", &s.Saturation, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat("Contrast", &s.Contrast, 0.5f, 2.0f, "%.2f");
	ImGui::SliderFloat("Vignette", &s.VignetteIntensity, 0.0f, 2.0f, "%.2f");
	if (ImGui::Button("Reset grading"))
	{
		s.WhiteBalanceTemperature = defaults.WhiteBalanceTemperature;
		s.WhiteBalanceTint = defaults.WhiteBalanceTint;
		s.Saturation = defaults.Saturation;
		s.Contrast = defaults.Contrast;
		s.VignetteIntensity = defaults.VignetteIntensity;
	}

	ImGui::PopID();
}
}
