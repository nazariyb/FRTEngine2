#include "Graphics/Render/Passes/Denoiser.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include "imgui.h"

#include "Config/Config.h"
#include "Profiler/Profiler.h"
#include "Shared/DenoiserData.h"


namespace frt::graphics
{
namespace
{
constexpr uint32 kMaxAtrousIterations = 8u;
constexpr uint32 kMaxHistoryLengthLimit = 255u;     // lengths are exact integers in fp16 up to 2048
constexpr float kReferenceHeight = 1080.0f;          // HistoryFixRadius is specified at this height

// Final state of everything the renderer reads back (outputs, debug texture).
constexpr D3D12_RESOURCE_STATES kShaderReadState =
	D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// Indexed by a-trous iteration.
constexpr FStringLiteral kAtrousScopes[kMaxAtrousIterations] = {
	"Denoiser_Atrous0", "Denoiser_Atrous1", "Denoiser_Atrous2", "Denoiser_Atrous3",
	"Denoiser_Atrous4", "Denoiser_Atrous5", "Denoiser_Atrous6", "Denoiser_Atrous7" };

rhi::STextureDesc MakeTarget (uint32 Width, uint32 Height, DXGI_FORMAT Format, const wchar_t* Name)
{
	rhi::STextureDesc desc;
	desc.Width = Width;
	desc.Height = Height;
	desc.Format = Format;
	desc.bSrv = true;
	desc.bUav = true;
	desc.InitialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	desc.Name = Name;
	return desc;
}

bool IsFinite (const DirectX::XMFLOAT3& V)
{
	return std::isfinite(V.x) && std::isfinite(V.y) && std::isfinite(V.z);
}

// Camera rays through screen uv (0, 0 = top-left), scaled to unit view depth along Forward:
// position relative to the camera = viewZ * (C + u * X + v * Y). Exact for any (also off-centre)
// perspective projection, because the unit-depth plane is an affine image of the screen.
struct SCameraRays
{
	DirectX::XMFLOAT3 X = {};
	DirectX::XMFLOAT3 Y = {};
	DirectX::XMFLOAT3 C = {};
};

bool ComputeCameraRays (const gpu::float4x4& ViewProj, const gpu::float3& Position, DirectX::FXMVECTOR Forward, SCameraRays& Out)
{
	using namespace DirectX;
	XMVECTOR determinant;
	const XMMATRIX inverse = XMMatrixInverse(&determinant, XMLoadFloat4x4(&ViewProj));
	const float det = XMVectorGetX(determinant);
	if (!std::isfinite(det) || std::abs(det) < 1e-30f)
	{
		return false;
	}
	const XMVECTOR origin = XMLoadFloat3(&Position);
	XMVECTOR dirs[3];
	const float uvs[3][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f } };
	for (uint32 i = 0u; i < 3u; ++i)
	{
		// Same unprojection as the path tracer's primary rays (NDC y up, any depth on the ray).
		const XMVECTOR ndc = XMVectorSet(uvs[i][0] * 2.0f - 1.0f, 1.0f - uvs[i][1] * 2.0f, 0.5f, 1.0f);
		const XMVECTOR direction = XMVectorSubtract(XMVector3TransformCoord(ndc, inverse), origin);
		const float depth = XMVectorGetX(XMVector3Dot(direction, Forward));
		if (!(std::abs(depth) > 1e-12f))
		{
			return false;
		}
		dirs[i] = XMVectorScale(direction, 1.0f / depth);
	}
	XMStoreFloat3(&Out.C, dirs[0]);
	XMStoreFloat3(&Out.X, XMVectorSubtract(dirs[1], dirs[0]));
	XMStoreFloat3(&Out.Y, XMVectorSubtract(dirs[2], dirs[0]));
	return IsFinite(Out.C) && IsFinite(Out.X) && IsFinite(Out.Y);
}

// Rows of the inverse of the matrix with columns X, Y, C (maps a camera-relative position to
// (z u, z v, z)): for A = [a b c], A^-1 rows are (b x c, c x a, a x b) / (a . (b x c)).
bool InvertRays (const SCameraRays& Rays, DirectX::XMFLOAT3& Row0, DirectX::XMFLOAT3& Row1, DirectX::XMFLOAT3& Row2)
{
	using namespace DirectX;
	const XMVECTOR a = XMLoadFloat3(&Rays.X);
	const XMVECTOR b = XMLoadFloat3(&Rays.Y);
	const XMVECTOR c = XMLoadFloat3(&Rays.C);
	const XMVECTOR bc = XMVector3Cross(b, c);
	const float det = XMVectorGetX(XMVector3Dot(a, bc));
	if (!std::isfinite(det) || std::abs(det) < 1e-20f)
	{
		return false;
	}
	const float inv = 1.0f / det;
	XMStoreFloat3(&Row0, XMVectorScale(bc, inv));
	XMStoreFloat3(&Row1, XMVectorScale(XMVector3Cross(c, a), inv));
	XMStoreFloat3(&Row2, XMVectorScale(XMVector3Cross(a, b), inv));
	return IsFinite(Row0) && IsFinite(Row1) && IsFinite(Row2);
}

// Compute passes may read a texture in any read state that contains NON_PIXEL_SHADER_RESOURCE
// (e.g. the path tracer leaves its targets in NON_PIXEL | PIXEL); avoid a pointless barrier.
void MakeComputeReadable (rhi::CBarrierBatch& Barriers, rhi::SGpuTexture& Texture)
{
	if ((Texture.State & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0u)
	{
		Barriers.Transition(Texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}
}
}


struct CDenoiser::SImpl
{
	rhi::HPipeline TemporalPipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline HistoryFixPipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline AtrousFirstPipeline = rhi::InvalidPipelineHandle;  // iteration 0 permutation
	rhi::HPipeline AtrousPipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline PassthroughPipeline = rhi::InvalidPipelineHandle;

	uint32 Width = 0u;
	uint32 Height = 0u;
	uint64 MemoryBytes = 0u;

	// Ping-pong state: [Current] is written this frame, [Current ^ 1] holds last frame's.
	rhi::SGpuTexture Guide[2];          // RG32_UINT: oct normal 12:12 + roughness 8, viewZ f32
	rhi::SGpuTexture DiffHistory[2];    // RGBA16F: slow history rgb, a = history length
	rhi::SGpuTexture SpecHistory[2];
	rhi::SGpuTexture Fast[2];           // RGBA16F fast history: (diffuse lum, diffuse hit, specular lum, specular hit)
	rhi::SGpuTexture MomentsHistory;    // RGBA16F filtered moments: diffuse (m1, sqrt m2), specular (m1, sqrt m2)
	rhi::SGpuTexture MomentsAccum;      // this frame's accumulated moments (transient), same encoding

	// Per-frame transients: [0] temporal result / a-trous ping, [1] history-fix result / pong.
	// [0] temporal result, [1] history-fix result (young pixels only), [2] a-trous ping-pong.
	rhi::SGpuTexture DiffTemp[3];
	rhi::SGpuTexture SpecTemp[3];
	rhi::SGpuTexture Variance;          // RGBA16F: luminance variance (diffuse, specular), firefly scales
	rhi::SGpuTexture AtrousAux;         // R11G11B10F: lengths + specular footprint for a-trous iterations 1+
	rhi::SGpuTexture HistoryLength;     // R16F debug
	rhi::SGpuTexture YoungTiles;        // R8_UINT per 8x8 tile: has histories for the fix pass
	rhi::SGpuTexture AntilagTiles[2];   // RGBA32F per 8x8 tile: anti-lag sums, diffuse / specular

	rhi::SGpuTexture DiffOutput;
	rhi::SGpuTexture SpecOutput;

	uint32 Current = 0u;
	bool bGuideValid = false;
	bool bDiffValid = false;
	bool bSpecValid = false;
	bool bResetRequested = false;
	bool bHasLastFrame = false;
	uint64 LastFrameNumber = 0u;
	DirectX::XMFLOAT3 LastForward = { 0.0f, 0.0f, 1.0f };

	void InvalidateHistory ()
	{
		bGuideValid = false;
		bDiffValid = false;
		bSpecValid = false;
	}

	template <typename F>
	void ForEachTexture (F&& Function)
	{
		for (uint32 i = 0u; i < 2u; ++i)
		{
			Function(Guide[i]);
			Function(DiffHistory[i]);
			Function(SpecHistory[i]);
			Function(Fast[i]);
		}
		for (uint32 i = 0u; i < 3u; ++i)
		{
			Function(DiffTemp[i]);
			Function(SpecTemp[i]);
		}
		Function(MomentsHistory);
		Function(MomentsAccum);
		Function(Variance);
		Function(AtrousAux);
		Function(HistoryLength);
		Function(YoungTiles);
		Function(AntilagTiles[0]);
		Function(AntilagTiles[1]);
		Function(DiffOutput);
		Function(SpecOutput);
	}

	void DestroyTextures (rhi::CDevice& Device)
	{
		ForEachTexture([&Device] (rhi::SGpuTexture& Texture)
		{
			if (Texture.IsValid())
			{
				Device.Destroy(Texture);
			}
		});
		Width = 0u;
		Height = 0u;
		MemoryBytes = 0u;
		InvalidateHistory();
	}
};


void CDenoiser::Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines)
{
	(void)Device;
	if (!Impl)
	{
		Impl = new SImpl();
	}
	Impl->TemporalPipeline = Pipelines.CreateCompute("Denoiser_Temporal", MakeShaderDesc("Denoiser/Temporal.hlsl", "main", "cs_6_6"));
	Impl->HistoryFixPipeline = Pipelines.CreateCompute("Denoiser_HistoryFix", MakeShaderDesc("Denoiser/HistoryFix.hlsl", "main", "cs_6_6"));
	Impl->AtrousFirstPipeline = Pipelines.CreateCompute("Denoiser_AtrousFirst", MakeShaderDesc("Denoiser/Atrous.hlsl", "main", "cs_6_6", { { "ATROUS_FIRST", "1" } }));
	Impl->AtrousPipeline = Pipelines.CreateCompute("Denoiser_Atrous", MakeShaderDesc("Denoiser/Atrous.hlsl", "main", "cs_6_6", { { "ATROUS_FIRST", "0" } }));
	Impl->PassthroughPipeline = Pipelines.CreateCompute("Denoiser_Passthrough", MakeShaderDesc("Denoiser/Passthrough.hlsl", "main", "cs_6_6"));
}

void CDenoiser::Shutdown (rhi::CDevice& Device)
{
	if (!Impl)
	{
		return;
	}
	Impl->DestroyTextures(Device);
	delete Impl;
	Impl = nullptr;
}

void CDenoiser::Resize (rhi::CDevice& Device, uint32 Width, uint32 Height)
{
	if (!Impl)
	{
		return;
	}
	SImpl& s = *Impl;
	Width = (std::max)(Width, 1u);
	Height = (std::max)(Height, 1u);
	if (Width == s.Width && Height == s.Height && s.DiffOutput.IsValid())
	{
		return;
	}
	s.DestroyTextures(Device);
	s.Width = Width;
	s.Height = Height;

	const DXGI_FORMAT rgba16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
	const wchar_t* const guideNames[2] = { L"Denoiser_Guide0", L"Denoiser_Guide1" };
	const wchar_t* const diffHistoryNames[2] = { L"Denoiser_DiffHistory0", L"Denoiser_DiffHistory1" };
	const wchar_t* const specHistoryNames[2] = { L"Denoiser_SpecHistory0", L"Denoiser_SpecHistory1" };
	const wchar_t* const fastNames[2] = { L"Denoiser_Fast0", L"Denoiser_Fast1" };
	const wchar_t* const diffTempNames[3] = { L"Denoiser_DiffTemp0", L"Denoiser_DiffTemp1", L"Denoiser_DiffTemp2" };
	const wchar_t* const specTempNames[3] = { L"Denoiser_SpecTemp0", L"Denoiser_SpecTemp1", L"Denoiser_SpecTemp2" };
	for (uint32 i = 0u; i < 2u; ++i)
	{
		s.Guide[i] = Device.CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R32G32_UINT, guideNames[i]));
		s.DiffHistory[i] = Device.CreateTexture(MakeTarget(Width, Height, rgba16, diffHistoryNames[i]));
		s.SpecHistory[i] = Device.CreateTexture(MakeTarget(Width, Height, rgba16, specHistoryNames[i]));
		s.Fast[i] = Device.CreateTexture(MakeTarget(Width, Height, rgba16, fastNames[i]));
	}
	for (uint32 i = 0u; i < 3u; ++i)
	{
		s.DiffTemp[i] = Device.CreateTexture(MakeTarget(Width, Height, rgba16, diffTempNames[i]));
		s.SpecTemp[i] = Device.CreateTexture(MakeTarget(Width, Height, rgba16, specTempNames[i]));
	}
	s.MomentsHistory = Device.CreateTexture(MakeTarget(Width, Height, rgba16, L"Denoiser_MomentsHistory"));
	s.MomentsAccum = Device.CreateTexture(MakeTarget(Width, Height, rgba16, L"Denoiser_MomentsAccum"));
	s.Variance = Device.CreateTexture(MakeTarget(Width, Height, rgba16, L"Denoiser_Variance"));
	s.AtrousAux = Device.CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R11G11B10_FLOAT, L"Denoiser_AtrousAux"));
	s.HistoryLength = Device.CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R16_FLOAT, L"Denoiser_HistoryLength"));
	const uint32 tilesX = (Width + 7u) / 8u;
	const uint32 tilesY = (Height + 7u) / 8u;
	s.YoungTiles = Device.CreateTexture(MakeTarget(tilesX, tilesY, DXGI_FORMAT_R8_UINT, L"Denoiser_YoungTiles"));
	s.AntilagTiles[0] = Device.CreateTexture(MakeTarget(tilesX, tilesY, DXGI_FORMAT_R32G32B32A32_FLOAT, L"Denoiser_AntilagTilesD"));
	s.AntilagTiles[1] = Device.CreateTexture(MakeTarget(tilesX, tilesY, DXGI_FORMAT_R32G32B32A32_FLOAT, L"Denoiser_AntilagTilesS"));
	s.DiffOutput = Device.CreateTexture(MakeTarget(Width, Height, rgba16, L"Denoiser_DiffuseOutput"));
	s.SpecOutput = Device.CreateTexture(MakeTarget(Width, Height, rgba16, L"Denoiser_SpecularOutput"));

	uint64 bytes = 0u;
	s.ForEachTexture([&bytes] (rhi::SGpuTexture& Texture)
	{
		bytes += uint64(Texture.Width) * Texture.Height * rhi::GetFormatBytesPerPixel(Texture.Format);
	});
	s.MemoryBytes = bytes;
	s.Current = 0u;
	s.InvalidateHistory();
}

void CDenoiser::Execute (CRenderContext& Context, const SDenoiserInputs& Inputs)
{
	if (!Impl || !Context.Device || !Context.Pipelines || !Context.CommandList || !Context.Frame)
	{
		return;
	}
	if (!Inputs.Diffuse || !Inputs.Specular || !Inputs.NormalRoughness || !Inputs.ViewZ || !Inputs.MotionVectors)
	{
		return;
	}
	SImpl& s = *Impl;
	const uint32 width = Inputs.Diffuse->Width;
	const uint32 height = Inputs.Diffuse->Height;
	if (width == 0u || height == 0u)
	{
		return;
	}
	if (width != s.Width || height != s.Height || !s.DiffOutput.IsValid())
	{
		Resize(*Context.Device, width, height);
	}

	FRT_GPU_SCOPE(Context.CommandList, "Denoiser");
	// Re-bind: makes the pass independent of what ran before it (ImGui, ray tracing).
	Context.BindCompute();

	rhi::CBarrierBatch& barriers = Context.Barriers;
	for (rhi::SGpuTexture* input : { Inputs.Diffuse, Inputs.Specular, Inputs.NormalRoughness, Inputs.ViewZ, Inputs.MotionVectors })
	{
		MakeComputeReadable(barriers, *input);
	}
	auto read = [&barriers] (rhi::SGpuTexture& Texture) { barriers.Transition(Texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); };
	auto write = [&barriers] (rhi::SGpuTexture& Texture) { barriers.Transition(Texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); };

	const gpu::SFrameConstants& frame = *Context.Frame;
	const uint32 cur = s.Current;
	const uint32 prev = cur ^ 1u;

	gpu::SDenoiserConstants c = {};
	c.InDiffuse = Inputs.Diffuse->Srv;
	c.InSpecular = Inputs.Specular->Srv;
	c.InNormalRoughness = Inputs.NormalRoughness->Srv;
	c.InViewZ = Inputs.ViewZ->Srv;
	c.InMotionVectors = Inputs.MotionVectors->Srv;
	c.Size = { width, height };
	c.InvSize = { 1.0f / float(width), 1.0f / float(height) };
	c.Jitter = frame.Jitter;
	c.PrevJitter = frame.PrevJitter;
	c.FrameIndex = frame.FrameIndex;
	c.HistoryDebugSignal = Settings.HistoryDebugSignal != 0u ? 1u : 0u;
	c.PrevGuide = s.Guide[prev].Srv;
	c.PrevDiffHistory = s.DiffHistory[prev].Srv;
	c.PrevSpecHistory = s.SpecHistory[prev].Srv;
	c.PrevFast = s.Fast[prev].Srv;
	c.PrevMoments = s.MomentsHistory.Srv;
	c.CurGuide = s.Guide[cur].Srv;
	c.CurFast = s.Fast[cur].Srv;
	c.CurMoments = s.MomentsAccum.Srv;
	c.OutGuide = s.Guide[cur].Uav;
	c.OutFast = s.Fast[cur].Uav;
	c.OutMoments = s.MomentsAccum.Uav;
	c.OutMomentsHistory = s.MomentsHistory.Uav;
	c.OutDiffHistory = s.DiffHistory[cur].Uav;
	c.OutSpecHistory = s.SpecHistory[cur].Uav;
	c.OutDiffuse = s.DiffOutput.Uav;
	c.OutSpecular = s.SpecOutput.Uav;
	c.AccumDiff = s.DiffTemp[0].Srv;
	c.AccumSpec = s.SpecTemp[0].Srv;
	c.AccumDiffUav = s.DiffTemp[0].Uav;
	c.AccumSpecUav = s.SpecTemp[0].Uav;
	c.FixedDiff = s.DiffTemp[1].Srv;
	c.FixedSpec = s.SpecTemp[1].Srv;
	c.FixedDiffUav = s.DiffTemp[1].Uav;
	c.FixedSpecUav = s.SpecTemp[1].Uav;
	c.Variance = s.Variance.Srv;
	c.VarianceUav = s.Variance.Uav;
	c.AtrousAux = s.AtrousAux.Srv;
	c.AtrousAuxUav = s.AtrousAux.Uav;
	c.HistoryLengthUav = s.HistoryLength.Uav;
	c.YoungTiles = s.YoungTiles.Srv;
	c.AntilagTilesD = s.AntilagTiles[0].Srv;
	c.AntilagTilesS = s.AntilagTiles[1].Srv;
	c.AntilagTilesDUav = s.AntilagTiles[0].Uav;
	c.AntilagTilesSUav = s.AntilagTiles[1].Uav;
	c.PrevDiffOutput = s.DiffOutput.Srv;
	c.PrevSpecOutput = s.SpecOutput.Srv;
	c.YoungTilesUav = s.YoungTiles.Uav;

	rhi::CPipelineLibrary& pipelines = *Context.Pipelines;
	const bool bPipelinesReady = pipelines.Get(s.TemporalPipeline) && pipelines.Get(s.HistoryFixPipeline) &&
		pipelines.Get(s.AtrousFirstPipeline) && pipelines.Get(s.AtrousPipeline);
	const bool bAnySignal = Settings.bDenoiseDiffuse || Settings.bDenoiseSpecular;

	if (!Settings.bEnabled || !bAnySignal || !bPipelinesReady)
	{
		// Disabled (or shaders failed to compile): outputs = untouched inputs.
		FRT_GPU_SCOPE(Context.CommandList, "Denoiser_Passthrough");
		write(s.DiffOutput);
		write(s.SpecOutput);
		write(s.HistoryLength);
		Context.PushPassConstants(c);
		Context.Dispatch2D(s.PassthroughPipeline, width, height);
		barriers.Transition(s.DiffOutput, kShaderReadState);
		barriers.Transition(s.SpecOutput, kShaderReadState);
		barriers.Transition(s.HistoryLength, kShaderReadState);
		Context.FlushBarriers();
		s.InvalidateHistory();
		s.bHasLastFrame = false;
		return;
	}

	// --- History validity ----------------------------------------------------------------------
	bool bReset = Inputs.bResetHistory || s.bResetRequested;
	s.bResetRequested = false;
	// A frame without this pass in between leaves the history two frames old while the motion
	// vectors describe one frame: start over.
	if (s.bHasLastFrame && Context.FrameNumber != 0u &&
		(Context.FrameNumber > s.LastFrameNumber + 1u || Context.FrameNumber < s.LastFrameNumber))
	{
		bReset = true;
	}

	// --- Camera reconstruction -------------------------------------------------------------------
	using namespace DirectX;
	const XMVECTOR forward = XMVector3Normalize(XMLoadFloat3(&frame.CameraForward));
	// The previous view depth was measured along last frame's forward; fall back to the view
	// axis of the previous view-projection (its w column) when there is no last frame.
	XMVECTOR prevForward = XMVector3Normalize(XMVectorSet(
		frame.PrevViewProjNoJitter._14, frame.PrevViewProjNoJitter._24, frame.PrevViewProjNoJitter._34, 0.0f));
	if (s.bHasLastFrame)
	{
		prevForward = XMLoadFloat3(&s.LastForward);
	}
	SCameraRays curRays, prevRays;
	XMFLOAT3 prevProj0 = {}, prevProj1 = {}, prevProj2 = {};
	const bool bCurRays = ComputeCameraRays(frame.ViewProjNoJitter, frame.CameraPosition, forward, curRays);
	const bool bPrevRays = ComputeCameraRays(frame.PrevViewProjNoJitter, frame.PrevCameraPosition, prevForward, prevRays) &&
		InvertRays(prevRays, prevProj0, prevProj1, prevProj2);
	if (!bCurRays)
	{
		// Unusable camera: nothing can be reconstructed this frame, keep the inputs.
		curRays.X = { 1.0f, 0.0f, 0.0f };
		curRays.Y = { 0.0f, -1.0f, 0.0f };
		curRays.C = { -0.5f, 0.5f, 1.0f };
		bReset = true;
	}
	if (!bPrevRays)
	{
		prevRays = curRays;
		if (!InvertRays(prevRays, prevProj0, prevProj1, prevProj2))
		{
			// Not even the current rays invert (and a failed inversion may have written non-finite
			// rows): zero rows project every point to depth 0, which the temporal pass never
			// reprojects from - on top of the history reset below.
			prevProj0 = prevProj1 = prevProj2 = {};
		}
		bReset = true;
	}
	c.CurRayX = curRays.X;
	c.CurRayY = curRays.Y;
	c.CurRayC = curRays.C;
	c.PixelAngle = XMVectorGetX(XMVector3Length(XMLoadFloat3(&curRays.Y))) / float(height);
	c.PrevRayX = prevRays.X;
	c.PrevRayY = prevRays.Y;
	c.PrevRayC = prevRays.C;
	c.PrevProj0 = prevProj0;
	c.PrevProj1 = prevProj1;
	c.PrevProj2 = prevProj2;
	c.CameraDelta = {
		frame.CameraPosition.x - frame.PrevCameraPosition.x,
		frame.CameraPosition.y - frame.PrevCameraPosition.y,
		frame.CameraPosition.z - frame.PrevCameraPosition.z };
	if (!IsFinite(c.CameraDelta))
	{
		c.CameraDelta = { 0.0f, 0.0f, 0.0f };
		bReset = true;
	}

	// --- Settings ------------------------------------------------------------------------------------
	const uint32 maxLength = std::clamp(Settings.MaxHistoryLength, 1u, kMaxHistoryLengthLimit);
	const uint32 maxFastLength = std::clamp(Settings.MaxFastHistoryLength, 1u, maxLength);
	const uint32 iterations = (std::min)(Settings.AtrousIterations, kMaxAtrousIterations);
	const uint32 fixFrames = (std::min)(Settings.HistoryFixFrames, maxLength);
	const float pi = 3.14159265358979f;
	c.MaxHistoryLength = float(maxLength);
	c.MaxFastHistoryLength = float(maxFastLength);
	c.DisocclusionThreshold = (std::max)(Settings.DisocclusionThreshold, 1e-4f);
	c.ReprojectionNormalCos = std::cos(std::clamp(Settings.ReprojectionNormalAngle, 0.0f, 180.0f) * (pi / 180.0f));
	c.SpecularParallaxScale = (std::max)(Settings.SpecularParallaxScale, 0.0f);
	c.MomentsMinAlpha = std::clamp(Settings.MomentsMinAlpha, 0.0f, 1.0f);
	c.HistoryFixFrames = float(fixFrames);
	c.HistoryFixRadius = std::clamp(Settings.HistoryFixRadius, 0.0f, 128.0f) * (float(height) / kReferenceHeight);
	c.AntilagSigma = (std::max)(Settings.AntilagSigma, 0.5f);
	c.AntilagSensitivity = (std::max)(Settings.AntilagSensitivity, 0.0f);
	c.FireflyThreshold = (std::max)(Settings.FireflyThreshold, 1.0f);
	c.LuminanceFloor = (std::max)(Settings.LuminanceFloor, 0.0f);
	c.SpecularNoiseTarget = (std::max)(Settings.SpecularNoiseTarget, 0.01f);
	c.SpecularNoiseRadiusMax = std::clamp(Settings.SpecularNoiseRadius, 0.0f, 128.0f) * (float(height) / kReferenceHeight);
	c.SpecularHitTolerance = (std::max)(Settings.SpecularHitTolerance, 0.01f);
	c.InputScale = Context.FrameNumber >= Settings.DebugInputScaleFrame ? (std::max)(Settings.DebugInputScale, 0.0f) : 1.0f;
	c.SpecularLobeFraction = (std::max)(Settings.SpecularLobeFraction, 0.01f);
	c.PhiNormal = (std::max)(Settings.PhiNormal, 0.0f);
	c.PhiDepth = (std::max)(Settings.PhiDepth, 1e-5f);
	c.PhiLuminanceDiffuse = (std::max)(Settings.PhiLuminanceDiffuse, 1e-3f);
	c.PhiLuminanceSpecular = (std::max)(Settings.PhiLuminanceSpecular, 1e-3f);
	c.RoughnessFraction = (std::max)(Settings.RoughnessFraction, 0.0f);
	c.SpecularFootprintScale = (std::max)(Settings.SpecularFootprintScale, 0.01f);

	uint32 flags = 0u;
	if (!bReset && s.bGuideValid) { flags |= gpu::DenoiserFlag_GuideHistoryValid; }
	if (!bReset && s.bDiffValid) { flags |= gpu::DenoiserFlag_DiffuseHistoryValid; }
	if (!bReset && s.bSpecValid) { flags |= gpu::DenoiserFlag_SpecularHistoryValid; }
	if (Settings.bDenoiseDiffuse) { flags |= gpu::DenoiserFlag_Diffuse; }
	if (Settings.bDenoiseSpecular) { flags |= gpu::DenoiserFlag_Specular; }
	if (Settings.bVirtualMotionSpecular) { flags |= gpu::DenoiserFlag_VirtualMotion; }
	if (Settings.bCatmullRomHistory) { flags |= gpu::DenoiserFlag_CatmullRom; }
	if (Settings.bJitterCompensation) { flags |= gpu::DenoiserFlag_JitterCompensation; }
	if (Settings.bHistoryFix && fixFrames > 1u) { flags |= gpu::DenoiserFlag_HistoryFix; }
	if (Settings.bAntiLag && Settings.AntilagSensitivity > 0.0f) { flags |= gpu::DenoiserFlag_AntiLag; }
	if (Settings.bFireflySuppression && Settings.FireflyThreshold > 0.0f) { flags |= gpu::DenoiserFlag_Firefly; }
	if (Settings.bFeedbackFirstIteration) { flags |= gpu::DenoiserFlag_Feedback; }
	if (Settings.bFeedbackMoments) { flags |= gpu::DenoiserFlag_FeedbackMoments; }
	if (Settings.bAtrousRotation) { flags |= gpu::DenoiserFlag_AtrousRotation; }
	if (Settings.bHistoryLengthDebug) { flags |= gpu::DenoiserFlag_HistoryLengthDebug; }
	if (iterations == 0u) { flags |= gpu::DenoiserFlag_NoAtrous; }
	if (Settings.OutputSignal == 1u) { flags |= gpu::DenoiserFlag_OutputDiffuseOnly; }
	if (Settings.OutputSignal == 2u) { flags |= gpu::DenoiserFlag_OutputSpecularOnly; }
	if (Settings.OutputSignal == 3u) { flags |= gpu::DenoiserFlag_OutputDiffuseOnly | gpu::DenoiserFlag_OutputSpecularOnly; }
	c.Flags = flags;

	Context.PushPassConstants(c);

	// --- 1. Temporal accumulation, anti-lag, noise estimate --------------------------------------------
	{
		FRT_GPU_SCOPE(Context.CommandList, "Denoiser_Temporal");
		for (rhi::SGpuTexture* t : { &s.Guide[prev], &s.DiffHistory[prev], &s.SpecHistory[prev], &s.Fast[prev], &s.MomentsHistory })
		{
			read(*t);
		}
		// Last frame's outputs (firefly reference) are still in their shader-read state.
		MakeComputeReadable(barriers, s.DiffOutput);
		MakeComputeReadable(barriers, s.SpecOutput);
		for (rhi::SGpuTexture* t : { &s.Guide[cur], &s.Fast[cur], &s.MomentsAccum, &s.DiffTemp[0], &s.SpecTemp[0],
			&s.Variance, &s.HistoryLength, &s.YoungTiles, &s.AntilagTiles[0], &s.AntilagTiles[1] })
		{
			write(*t);
		}
		Context.Dispatch2D(s.TemporalPipeline, width, height);
	}

	// --- 2. History fix (young pixels only) -------------------------------------------------------------
	{
		FRT_GPU_SCOPE(Context.CommandList, "Denoiser_HistoryFix");
		for (rhi::SGpuTexture* t : { &s.Guide[cur], &s.Fast[cur], &s.MomentsAccum, &s.DiffTemp[0], &s.SpecTemp[0], &s.YoungTiles })
		{
			read(*t);
		}
		write(s.DiffTemp[1]);
		write(s.SpecTemp[1]);
		if (iterations == 0u)
		{
			// No spatial filter: this pass writes the history and the outputs itself.
			for (rhi::SGpuTexture* t : { &s.DiffHistory[cur], &s.SpecHistory[cur], &s.MomentsHistory, &s.DiffOutput, &s.SpecOutput })
			{
				write(*t);
			}
		}
		Context.Dispatch2D(s.HistoryFixPipeline, width, height);
	}

	// --- 3. A-trous wavelet filter ------------------------------------------------------------------
	// Iteration 0 reads Temp[0] (temporal result) and Temp[1] (history fix, young pixels) and writes
	// Temp[2]; the later ones ping-pong between Temp[2] and Temp[0]; the last iteration writes the
	// outputs. Iteration 0 also writes the history, the moments history and the aux texture.
	rhi::SGpuTexture* pingDiff[2] = { &s.DiffTemp[2], &s.DiffTemp[0] };
	rhi::SGpuTexture* pingSpec[2] = { &s.SpecTemp[2], &s.SpecTemp[0] };
	for (uint32 i = 0u; i < iterations; ++i)
	{
		FRT_GPU_SCOPE(Context.CommandList, kAtrousScopes[i]);
		const bool bFirst = i == 0u;
		const bool bLast = i + 1u == iterations;
		rhi::SGpuTexture& inDiff = bFirst ? s.DiffTemp[0] : *pingDiff[(i - 1u) % 2u];
		rhi::SGpuTexture& inSpec = bFirst ? s.SpecTemp[0] : *pingSpec[(i - 1u) % 2u];
		rhi::SGpuTexture& outDiff = bLast ? s.DiffOutput : *pingDiff[i % 2u];
		rhi::SGpuTexture& outSpec = bLast ? s.SpecOutput : *pingSpec[i % 2u];
		read(inDiff);
		read(inSpec);
		write(outDiff);
		write(outSpec);
		if (bFirst)
		{
			for (rhi::SGpuTexture* t : { &s.DiffTemp[1], &s.SpecTemp[1], &s.Variance, &s.AntilagTiles[0], &s.AntilagTiles[1] })
			{
				read(*t);
			}
			for (rhi::SGpuTexture* t : { &s.DiffHistory[cur], &s.SpecHistory[cur], &s.MomentsHistory, &s.AtrousAux })
			{
				write(*t);
			}
		}
		else
		{
			read(s.AtrousAux);
		}
		if (bLast && Settings.OutputSignal == 3u && Settings.bHistoryLengthDebug)
		{
			// The history-state debug output reads the history-length texture the temporal pass
			// wrote through its UAV: order the two accesses.
			barriers.Uav(s.HistoryLength);
		}

		gpu::SDenoiserAtrousRootConstants root = {};
		root.StepSize = 1u << i;
		root.InDiffuse = inDiff.Srv;
		root.InSpecular = inSpec.Srv;
		root.OutDiffuse = outDiff.Uav;
		root.OutSpecular = outSpec.Uav;
		root.IterationFlags = (bFirst ? gpu::DenoiserAtrous_First : 0u) | (bLast ? gpu::DenoiserAtrous_Last : 0u);
		static_assert(sizeof(root) % sizeof(uint32) == 0u && sizeof(root) / sizeof(uint32) <= rhi::RootConstantCount);
		Context.SetRootConstants(reinterpret_cast<const uint32*>(&root), sizeof(root) / sizeof(uint32));
		Context.Dispatch2D(bFirst ? s.AtrousFirstPipeline : s.AtrousPipeline, width, height);
	}

	barriers.Transition(s.DiffOutput, kShaderReadState);
	barriers.Transition(s.SpecOutput, kShaderReadState);
	barriers.Transition(s.HistoryLength, kShaderReadState);
	Context.FlushBarriers();

	// --- Bookkeeping ------------------------------------------------------------------------------
	s.Current = prev;
	s.bGuideValid = bCurRays;
	s.bDiffValid = bCurRays && Settings.bDenoiseDiffuse;
	s.bSpecValid = bCurRays && Settings.bDenoiseSpecular;
	s.bHasLastFrame = true;
	s.LastFrameNumber = Context.FrameNumber;
	XMStoreFloat3(&s.LastForward, forward);
}

rhi::SGpuTexture& CDenoiser::GetDiffuseOutput ()
{
	static rhi::SGpuTexture empty;
	return Impl ? Impl->DiffOutput : empty;
}

rhi::SGpuTexture& CDenoiser::GetSpecularOutput ()
{
	static rhi::SGpuTexture empty;
	return Impl ? Impl->SpecOutput : empty;
}

rhi::SGpuTexture* CDenoiser::GetHistoryLengthTexture ()
{
	return (Impl && Impl->HistoryLength.IsValid()) ? &Impl->HistoryLength : nullptr;
}

void CDenoiser::DrawSettingsUi (SDenoiserSettings& Edit)
{
	auto sliderUint = [] (const char* Label, uint32& Value, int Min, int Max)
	{
		int v = static_cast<int>(Value);
		if (ImGui::SliderInt(Label, &v, Min, Max))
		{
			Value = static_cast<uint32>(std::clamp(v, Min, Max));
		}
	};
	constexpr ImGuiSliderFlags kLog = ImGuiSliderFlags_Logarithmic;

	ImGui::Checkbox("Enabled", &Edit.bEnabled);
	ImGui::SameLine();
	ImGui::Checkbox("Diffuse", &Edit.bDenoiseDiffuse);
	ImGui::SameLine();
	ImGui::Checkbox("Specular", &Edit.bDenoiseSpecular);
	ImGui::SameLine();
	if (ImGui::Button("Reset history") && Impl)
	{
		Impl->bResetRequested = true;
	}
	if (Impl && Impl->Width > 0u)
	{
		ImGui::TextDisabled("%u x %u, %.1f MB", Impl->Width, Impl->Height, double(Impl->MemoryBytes) / (1024.0 * 1024.0));
	}

	ImGui::SeparatorText("Temporal accumulation");
	sliderUint("Max history (frames)", Edit.MaxHistoryLength, 1, 128);
	sliderUint("Max fast history (frames)", Edit.MaxFastHistoryLength, 1, 16);
	ImGui::SliderFloat("Disocclusion threshold", &Edit.DisocclusionThreshold, 0.001f, 0.1f, "%.4f", kLog);
	ImGui::SliderFloat("Reprojection normal angle", &Edit.ReprojectionNormalAngle, 5.0f, 180.0f, "%.0f deg");
	ImGui::Checkbox("Catmull-Rom history", &Edit.bCatmullRomHistory);
	ImGui::SameLine();
	ImGui::Checkbox("Jitter compensation", &Edit.bJitterCompensation);
	ImGui::Checkbox("Specular virtual motion", &Edit.bVirtualMotionSpecular);
	ImGui::SliderFloat("Specular parallax sensitivity", &Edit.SpecularParallaxScale, 0.0f, 4.0f, "%.2f");
	ImGui::SliderFloat("Moments min alpha (0 = history rate)", &Edit.MomentsMinAlpha, 0.0f, 1.0f, "%.3f");

	ImGui::SeparatorText("History fix");
	ImGui::Checkbox("History fix", &Edit.bHistoryFix);
	sliderUint("History fix frames", Edit.HistoryFixFrames, 0, 16);
	ImGui::SliderFloat("History fix radius (px @1080p)", &Edit.HistoryFixRadius, 1.0f, 64.0f, "%.1f");

	ImGui::SeparatorText("Anti-lag / fireflies");
	ImGui::Checkbox("Anti-lag (40x40 significance test)", &Edit.bAntiLag);
	ImGui::SliderFloat("Anti-lag sigma", &Edit.AntilagSigma, 1.0f, 16.0f, "%.1f", kLog);
	ImGui::SliderFloat("Anti-lag sensitivity", &Edit.AntilagSensitivity, 0.0f, 4.0f, "%.2f");
	ImGui::Checkbox("Input firefly limit", &Edit.bFireflySuppression);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
	ImGui::SliderFloat("x mean##firefly", &Edit.FireflyThreshold, 10.0f, 100000.0f, "%.0f", kLog);

	ImGui::SeparatorText("Spatial (a-trous)");
	sliderUint("Iterations", Edit.AtrousIterations, 0, static_cast<int>(kMaxAtrousIterations));
	ImGui::Checkbox("Recurrent colour (feed back iteration 0)", &Edit.bFeedbackFirstIteration);
	ImGui::SameLine();
	ImGui::Checkbox("Recurrent moments", &Edit.bFeedbackMoments);
	ImGui::Checkbox("Rotate taps (iterations 1+)", &Edit.bAtrousRotation);
	ImGui::SliderFloat("Phi normal", &Edit.PhiNormal, 1.0f, 256.0f, "%.1f", kLog);
	ImGui::SliderFloat("Phi depth (plane / viewZ)", &Edit.PhiDepth, 0.001f, 0.2f, "%.4f", kLog);
	ImGui::SliderFloat("Phi luminance diffuse", &Edit.PhiLuminanceDiffuse, 0.1f, 32.0f, "%.2f", kLog);
	ImGui::SliderFloat("Phi luminance specular", &Edit.PhiLuminanceSpecular, 0.1f, 32.0f, "%.2f", kLog);
	ImGui::SliderFloat("Luminance floor (x local mean)", &Edit.LuminanceFloor, 0.0f, 4.0f, "%.2f");
	ImGui::SliderFloat("Specular lobe fraction", &Edit.SpecularLobeFraction, 0.05f, 2.0f, "%.2f");
	ImGui::SliderFloat("Roughness tolerance", &Edit.RoughnessFraction, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Specular footprint scale", &Edit.SpecularFootprintScale, 0.25f, 16.0f, "%.2f", kLog);
	ImGui::SliderFloat("Specular noise target", &Edit.SpecularNoiseTarget, 0.05f, 2.0f, "%.2f", kLog);
	ImGui::SliderFloat("Specular noise radius (px @1080p)", &Edit.SpecularNoiseRadius, 0.0f, 64.0f, "%.1f");
	ImGui::SliderFloat("Specular hit-distance tolerance", &Edit.SpecularHitTolerance, 0.02f, 4.0f, "%.2f", kLog);

	ImGui::SeparatorText("Debug");
	ImGui::Checkbox("Write history-length texture", &Edit.bHistoryLengthDebug);
	static const char* kSignals[] = { "Diffuse", "Specular" };
	int signal = Edit.HistoryDebugSignal != 0u ? 1 : 0;
	if (ImGui::Combo("History length view", &signal, kSignals, static_cast<int>(std::size(kSignals))))
	{
		Edit.HistoryDebugSignal = static_cast<uint32>(signal);
	}
	static const char* kOutputs[] = { "Diffuse + specular", "Diffuse only", "Specular only", "History state" };
	int output = static_cast<int>((std::min)(Edit.OutputSignal, 3u));
	if (ImGui::Combo("Output", &output, kOutputs, static_cast<int>(std::size(kOutputs))))
	{
		Edit.OutputSignal = static_cast<uint32>(output);
	}
}

void CDenoiser::ApplyConfig (const config::CConfig& Config, SDenoiserSettings& Out)
{
	SDenoiserSettings& s = Out;
	const std::pair<const char*, bool*> bools[] = {
		{ "Enabled", &s.bEnabled }, { "DenoiseDiffuse", &s.bDenoiseDiffuse }, { "DenoiseSpecular", &s.bDenoiseSpecular },
		{ "HistoryFix", &s.bHistoryFix }, { "AntiLag", &s.bAntiLag }, { "FireflySuppression", &s.bFireflySuppression },
		{ "VirtualMotionSpecular", &s.bVirtualMotionSpecular }, { "CatmullRomHistory", &s.bCatmullRomHistory },
		{ "JitterCompensation", &s.bJitterCompensation }, { "FeedbackFirstIteration", &s.bFeedbackFirstIteration },
		{ "AtrousRotation", &s.bAtrousRotation }, { "HistoryLengthDebug", &s.bHistoryLengthDebug },
		{ "FeedbackMoments", &s.bFeedbackMoments } };
	const std::pair<const char*, uint32*> uints[] = {
		{ "MaxHistoryLength", &s.MaxHistoryLength }, { "MaxFastHistoryLength", &s.MaxFastHistoryLength },
		{ "AtrousIterations", &s.AtrousIterations }, { "HistoryFixFrames", &s.HistoryFixFrames },
		{ "HistoryDebugSignal", &s.HistoryDebugSignal }, { "OutputSignal", &s.OutputSignal },
		{ "DebugInputScaleFrame", &s.DebugInputScaleFrame } };
	const std::pair<const char*, float*> floats[] = {
		{ "AntilagSigma", &s.AntilagSigma }, { "PhiNormal", &s.PhiNormal }, { "PhiDepth", &s.PhiDepth },
		{ "PhiLuminanceDiffuse", &s.PhiLuminanceDiffuse }, { "PhiLuminanceSpecular", &s.PhiLuminanceSpecular },
		{ "DisocclusionThreshold", &s.DisocclusionThreshold }, { "ReprojectionNormalAngle", &s.ReprojectionNormalAngle },
		{ "SpecularParallaxScale", &s.SpecularParallaxScale }, { "MomentsMinAlpha", &s.MomentsMinAlpha },
		{ "HistoryFixRadius", &s.HistoryFixRadius }, { "AntilagSensitivity", &s.AntilagSensitivity },
		{ "FireflyThreshold", &s.FireflyThreshold }, { "SpecularLobeFraction", &s.SpecularLobeFraction },
		{ "LuminanceFloor", &s.LuminanceFloor }, { "SpecularNoiseTarget", &s.SpecularNoiseTarget },
		{ "DebugInputScale", &s.DebugInputScale },
		{ "SpecularNoiseRadius", &s.SpecularNoiseRadius }, { "SpecularHitTolerance", &s.SpecularHitTolerance },
		{ "RoughnessFraction", &s.RoughnessFraction }, { "SpecularFootprintScale", &s.SpecularFootprintScale } };
	for (const auto& [key, value] : bools)
	{
		*value = Config.Get("Denoiser", key, *value);
	}
	for (const auto& [key, value] : uints)
	{
		*value = Config.Get("Denoiser", key, *value);
	}
	for (const auto& [key, value] : floats)
	{
		*value = Config.Get("Denoiser", key, *value);
	}
}
}
