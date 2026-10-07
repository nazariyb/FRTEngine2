// Final display pass: full-screen triangle into the back buffer.
//
//   exposure -> bloom (energy-conserving lerp) -> white balance x saturation (one 3x3) ->
//   contrast -> vignette -> tone mapper -> output encoding (exact sRGB OETF / scRGB) ->
//   triangular-PDF dither -> explicit quantisation to the RTV's bit depth.
//
// Grading runs on scene-linear values before the tone mapper (like a camera / film pipeline),
// so the tone mapper's highlight roll-off still applies to graded colours.

#include "Common/Bindings.hlsli"
#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "PostProcess/PostCommon.hlsli"
#include "PostProcess/ToneMapping.hlsli"
#include "Shared/PostProcessData.h"

ConstantBuffer<SPostFinalConstants> g_Pass : register(b2);

bool PassFlag (uint Flag) { return (g_Pass.Flags & Flag) != 0u; }

struct SVertexOutput
{
	float4 Position : SV_Position;
	nointerpolation float Exposure : EXPOSURE;   // uniform: fetched by 3 vertices, not 2M pixels
};

// One triangle (-1, 1), (3, 1), (-1, -3) covers the viewport: no diagonal seam and no helper
// lanes wasted along it, no vertex buffer.
SVertexOutput VSMain (uint VertexId : SV_VertexID)
{
	const float2 t = float2((VertexId << 1u) & 2u, VertexId & 2u);
	SVertexOutput output;
	output.Position = float4(t * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
	output.Exposure = LoadExposure(g_Pass.ExposureBuffer, g_Pass.FallbackExposure) * GetInvPreExposure();
	return output;
}

// Interleaved gradient noise (Jimenez 2014, "Next Generation Post Processing in Call of Duty:
// Advanced Warfare"): cheap, well distributed over 3x3 neighbourhoods, close to blue noise. The
// per-frame offset of 5.588238 px (Jimenez 2016) decorrelates consecutive frames so the eye /
// any later temporal filter averages the dither out.
float InterleavedGradientNoise (float2 Pixel, uint Frame)
{
	Pixel += 5.588238f * float(Frame & 63u);
	return frac(52.9829189f * frac(dot(Pixel, float2(0.06711056f, 0.00583715f))));
}

// Uniform [0, 1) -> triangular PDF on [-1, 1] through the inverse CDF, keeping the spectral
// (blue-ish) character of the source noise (Gjoel 2016, "Banding in Games", GDC). TPDF dither of
// +-1 LSB makes the quantisation error's mean *and* variance independent of the signal
// (Lipshitz, Wannamaker & Vanderkooy 1992), so gradients show neither banding nor noise
// modulation.
float TriangularNoise (float U)
{
	const float n = U * 2.0f - 1.0f;
	return sign(n) * (1.0f - sqrt(1.0f - abs(n)));
}

// Encoded [0, 1] -> exactly representable code values of the render target.
float3 DitherQuantize (float3 Encoded, float2 Pixel)
{
	const float levels = g_Pass.QuantizationLevels;
	float3 v = saturate(Encoded) * levels;
	if (PassFlag(FinalFlag_Dither))
	{
		const float noise = TriangularNoise(InterleavedGradientNoise(floor(Pixel), g_Frame.FrameIndex));
		// Within 1 LSB of black / white the +-1 LSB dither would be clipped on one side and bias
		// the mean (lifted blacks); shrink it there so the mean is preserved and 0 stays 0.
		const float3 amplitude = saturate(min(v, levels - v));
		v += noise * amplitude;
	}
	return clamp(round(v), 0.0f, levels) * (1.0f / levels);
}

// Display-linear [0, 1] -> render target value.
float3 OutputTransform (float3 DisplayLinear, float2 Pixel, bool bAlreadyEncoded)
{
	if (PassFlag(FinalFlag_ScRgbOutput))
	{
		// scRGB: linear Rec.709, 1.0 = 80 nits. SDR-graded image placed at paper white.
		const float3 linearValue = bAlreadyEncoded ? SrgbToLinear(saturate(DisplayLinear)) : DisplayLinear;
		return linearValue * g_Pass.ScRgbScale;
	}

	// Exact piecewise sRGB OETF (IEC 61966-2-1), not a 2.2 gamma.
	const float3 encoded = bAlreadyEncoded ? DisplayLinear : LinearToSrgb(saturate(DisplayLinear));
	float3 code = DitherQuantize(encoded, Pixel);
	if (PassFlag(FinalFlag_SrgbRenderTarget))
	{
		// The RTV re-encodes: hand back the linear value of the chosen code so the hardware's
		// OETF lands on it (within its 0.6 ULP conversion tolerance).
		code = SrgbToLinear(code);
	}
	return code;
}

float3 SampleDebug (float2 Pixel, float2 Uv)
{
	// Point sampling: debug views must show exact texels at any resolution ratio.
	const float3 value = Tex2D(g_Pass.DebugTexture).SampleLevel(s_PointClamp, Uv, 0.0f).rgb;
	return select(isnan(value), 0.0f, clamp(value, -65504.0f, 65504.0f));
}

// Bilinear resample of an HDR input whose size differs from the output, sanitising each texel
// before it is weighted: a hardware bilinear fetch would spread one NaN / Inf over the 2x2
// footprint (and 0 x Inf = NaN) before SanitizeHdr could catch it. Rare path (the renderer feeds
// display-size HDR), so four point loads are fine.
float3 SampleHdrSanitized (Texture2D<float4> Source, float2 Uv)
{
	uint width, height;
	Source.GetDimensions(width, height);
	const float2 position = Uv * float2(width, height) - 0.5f;
	const float2 base = floor(position);
	const float2 f = position - base;
	const int2 maxTexel = int2(width, height) - 1;
	const int2 p0 = clamp(int2(base), 0, maxTexel);
	const int2 p1 = clamp(int2(base) + 1, 0, maxTexel);
	const float3 c00 = SanitizeHdr(Source.Load(int3(p0.x, p0.y, 0)).rgb);
	const float3 c10 = SanitizeHdr(Source.Load(int3(p1.x, p0.y, 0)).rgb);
	const float3 c01 = SanitizeHdr(Source.Load(int3(p0.x, p1.y, 0)).rgb);
	const float3 c11 = SanitizeHdr(Source.Load(int3(p1.x, p1.y, 0)).rgb);
	return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

float3 Grade (float3 Color, float2 Uv)
{
	if (PassFlag(FinalFlag_Grading))
	{
		// White balance (Bradford CAT in LMS) and saturation, pre-multiplied on the CPU.
		Color = float3(dot(g_Pass.ColorMatrixR.xyz, Color), dot(g_Pass.ColorMatrixG.xyz, Color), dot(g_Pass.ColorMatrixB.xyz, Color));
		Color = max(Color, 0.0f);
		// Contrast: slope in log2 space pivoting on 18% grey, i.e. 0.18 * (c / 0.18)^k per
		// channel (the ASC CDL power / UE ColorContrast form). pow(0, k > 0) = 0.
		Color = 0.18f * pow(Color * (1.0f / 0.18f), g_Pass.Contrast);
	}

	if (PassFlag(FinalFlag_Vignette))
	{
		// Natural vignetting (cos^4 law): an off-axis image point at normalised radius r sees the
		// pupil under tan(theta) = k r, so E ~ cos^4(theta) = 1 / (1 + k^2 r^2)^2. r = 1 at the
		// corners, aspect-correct so the falloff is round.
		const float2 d = (Uv - 0.5f) * float2(g_Pass.AspectRatio, 1.0f);
		const float r2 = dot(d, d) / (0.25f * (g_Pass.AspectRatio * g_Pass.AspectRatio + 1.0f));
		const float k2 = g_Pass.VignetteStrength * g_Pass.VignetteStrength;
		Color *= 1.0f / Sq(1.0f + k2 * r2);
	}
	return Color;
}

float4 PSMain (SVertexOutput Input) : SV_Target0
{
	const float2 pixel = Input.Position.xy;   // pixel centre
	const float2 uv = pixel * g_Pass.InvOutputSize;

	if (PassFlag(FinalFlag_Debug))
	{
		const float3 value = SampleDebug(pixel, uv);
		if (PassFlag(FinalFlag_DebugLinear))
		{
			// Linear debug values. Data views (albedo, normals, roughness, ids, ...) are shown
			// exactly: OETF only, so 0.5 stays 0.5 (AgX alone would map white to ~0.53 and
			// desaturate). Radiance views opt into exposure + the tone mapper (no bloom / grading);
			// a tone mapper without exposure would show sunlit radiance as flat white.
			float3 display = saturate(value);
			if (PassFlag(FinalFlag_DebugToneMap))
			{
				display = ApplyToneMapper(max(value, 0.0f) * Input.Exposure, g_Pass.ToneMapper, g_Pass.AgXLook, g_Pass.ReinhardWhitePoint);
			}
			return float4(OutputTransform(display, pixel, false), 1.0f);
		}
		return float4(OutputTransform(saturate(value), pixel, true), 1.0f);
	}

	float3 hdr = 0.0f;
	if (PassFlag(FinalFlag_HasHdr))
	{
		Texture2D<float4> hdrTexture = Tex2D(g_Pass.HdrTexture);
		hdr = PassFlag(FinalFlag_HdrSameSize)
			? SanitizeHdr(hdrTexture.Load(int3(int2(pixel), 0)).rgb)
			: SampleHdrSanitized(hdrTexture, uv);
	}

	float3 color = hdr * Input.Exposure;

	if (PassFlag(FinalFlag_Bloom))
	{
		// The pyramid is built from the exposed image and stores the running mean of its levels
		// (BloomUpsample.hlsl), i.e. it is normalised to the image energy, so a lerp
		// redistributes light instead of adding it (Jimenez 2014).
		const float3 bloom = Tent3x3(Tex2D(g_Pass.BloomTexture), uv, g_Pass.BloomTapOffset);
		color = lerp(color, bloom, g_Pass.BloomIntensity);
	}

	color = Grade(color, uv);
	const float3 display = ApplyToneMapper(color, g_Pass.ToneMapper, g_Pass.AgXLook, g_Pass.ReinhardWhitePoint);
	return float4(OutputTransform(display, pixel, false), 1.0f);
}
