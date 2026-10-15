#include "Graphics/Render/Passes/RasterShadows.h"

#include <algorithm>
#include <cmath>

#include "Graphics/Render/Scene/GpuScene.h"
#include "Profiler/Profiler.h"


namespace frt::graphics
{
using namespace DirectX;

namespace
{
constexpr float kPiF = 3.14159265358979323846f;

std::vector<D3D12_INPUT_ELEMENT_DESC> MakeShadowVertexLayout ()
{
	// SVertex (GraphicsCoreTypes.h) read as position + uv; the stride comes from the draw.
	return {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
}

void StoreView (gpu::SRasterShadowView& Out, const XMMATRIX& ViewProj, float OffsetU, float OffsetV, float ScaleU, float ScaleV)
{
	XMStoreFloat4x4(&Out.ViewProj, ViewProj);
	Out.AtlasRect = { OffsetU, OffsetV, ScaleU, ScaleV };
}
}

bool IsBoxOutsideView (const XMMATRIX& ViewProj, const XMFLOAT3& Min, const XMFLOAT3& Max)
{
	uint32 outsideAll = 0x3Fu;
	for (uint32 c = 0; c < 8u; ++c)
	{
		const XMVECTOR corner = XMVectorSet((c & 1u) ? Max.x : Min.x, (c & 2u) ? Max.y : Min.y, (c & 4u) ? Max.z : Min.z, 1.0f);
		XMFLOAT4 clip;
		XMStoreFloat4(&clip, XMVector4Transform(corner, ViewProj));
		uint32 outside = 0u;
		outside |= clip.x < -clip.w ? 0x01u : 0u;
		outside |= clip.x > clip.w ? 0x02u : 0u;
		outside |= clip.y < -clip.w ? 0x04u : 0u;
		outside |= clip.y > clip.w ? 0x08u : 0u;
		outside |= clip.z < 0.0f ? 0x10u : 0u;
		outside |= clip.z > clip.w ? 0x20u : 0u;
		outsideAll &= outside;
		if (outsideAll == 0u)
		{
			return false;
		}
	}
	return outsideAll != 0u;
}

void CRasterShadows::Initialize (rhi::CDevice& InDevice, rhi::CPipelineLibrary& InPipelines)
{
	Device = &InDevice;
	Pipelines = &InPipelines;

	rhi::SGraphicsPipelineDesc desc;
	desc.VertexShader = MakeShaderDesc("Raster/Shadow.hlsl", "VsShadow", "vs_6_6");
	desc.PixelShader = MakeShaderDesc("Raster/Shadow.hlsl", "PsShadowOpaque", "ps_6_6");
	desc.InputLayout = MakeShadowVertexLayout();
	desc.RenderTargetCount = 0u;
	desc.DepthFormat = DXGI_FORMAT_D32_FLOAT;
	desc.bDepthTest = true;
	desc.bDepthWrite = true;
	desc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
	desc.CullMode = D3D12_CULL_MODE_NONE;   // thin and open meshes must shadow from both sides
	OpaquePipeline = Pipelines->CreateGraphics("RasterShadow_Opaque", desc);
	desc.PixelShader = MakeShaderDesc("Raster/Shadow.hlsl", "PsShadowMasked", "ps_6_6");
	MaskedPipeline = Pipelines->CreateGraphics("RasterShadow_Masked", desc);

	auto makeDepth = [&] (uint32 Width, uint32 Height, FStringView Name)
	{
		D3D12_CLEAR_VALUE clear = {};
		clear.Format = DXGI_FORMAT_D32_FLOAT;
		clear.DepthStencil.Depth = 0.0f;
		rhi::STextureDesc t;
		t.Width = Width;
		t.Height = Height;
		t.Format = DXGI_FORMAT_R32_TYPELESS;
		t.DsvFormat = DXGI_FORMAT_D32_FLOAT;
		t.SrvFormat = DXGI_FORMAT_R32_FLOAT;
		t.bDsv = true;
		t.bSrv = true;
		t.InitialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		t.ClearValue = &clear;
		t.Name = Name;
		return Device->CreateTexture(t);
	};
	SunShadow = makeDepth(SunShadowSize, SunShadowSize, "Raster_SunShadow");
	LocalShadow = makeDepth(6u * CubeTileSize, gpu::RasterMaxCubeShadows * CubeTileSize, "Raster_CubeShadows");
}

void CRasterShadows::Shutdown ()
{
	if (!Device)
	{
		return;
	}
	Device->Destroy(SunShadow);
	Device->Destroy(LocalShadow);
	Device = nullptr;
}

void CRasterShadows::RenderView (
	CRenderContext& Context,
	const CGpuScene& Scene,
	uint32 ViewIndex,
	const D3D12_VIEWPORT& Viewport,
	const XMMATRIX& ViewProj,
	uint32 SkipInstance)
{
	ID3D12GraphicsCommandList4* cmd = Context.CommandList;
	const D3D12_RECT scissor = {
		static_cast<LONG>(Viewport.TopLeftX), static_cast<LONG>(Viewport.TopLeftY),
		static_cast<LONG>(Viewport.TopLeftX + Viewport.Width), static_cast<LONG>(Viewport.TopLeftY + Viewport.Height) };
	cmd->RSSetViewports(1, &Viewport);
	cmd->RSSetScissorRects(1, &scissor);
	// Opaque casters first, then the alpha-tested ones: two pipeline switches per view.
	for (uint32 pass = 0; pass < 2u; ++pass)
	{
		ID3D12PipelineState* pso = Pipelines->Get(pass == 0u ? OpaquePipeline : MaskedPipeline);
		if (!pso)
		{
			continue;
		}
		cmd->SetPipelineState(pso);
		for (const SSceneDraw& draw : Scene.GetDraws())
		{
			const bool bMasked = draw.bAlphaTested || draw.bAlphaBlend;
			if (bMasked != (pass == 1u) || draw.bHelper || draw.InstanceIndex == SkipInstance || IsBoxOutsideView(ViewProj, draw.BoundsMin, draw.BoundsMax))
			{
				continue;
			}
			const gpu::SRasterShadowDrawConstants constants = { draw.InstanceIndex, draw.GeometryIndex, ViewIndex, 0u };
			cmd->SetGraphicsRoot32BitConstants(rhi::RootParam_Constants, 4, &constants, 0);
			cmd->IASetVertexBuffers(0, 1, &draw.VertexBuffer);
			cmd->IASetIndexBuffer(&draw.IndexBuffer);
			cmd->DrawIndexedInstanced(draw.IndexCount, 1, draw.IndexOffset, draw.VertexOffset, 0);
		}
	}
}

void CRasterShadows::Execute (CRenderContext& Context, const CGpuScene& Scene, bool bShadows, uint32 MaxEmitters, gpu::SRasterConstants& Constants)
{
	FRT_GPU_SCOPE(Context.CommandList, "RasterShadows");
	ID3D12GraphicsCommandList4* cmd = Context.CommandList;
	const gpu::SFrameConstants& frame = *Context.Frame;

	Constants.SunShadowMap = SunShadow.Srv;
	Constants.LocalShadowMap = LocalShadow.Srv;
	Constants.SunShadowTexel = { 1.0f / SunShadowSize, 1.0f / SunShadowSize };
	Constants.LocalShadowTexel = { 1.0f / LocalShadow.Width, 1.0f / LocalShadow.Height };
	Constants.LocalShadowTileTexels = static_cast<float>(CubeTileSize);

	// ---- sun: orthographic, fitted to the scene's world bounds as seen from the sun ----
	const bool bSun = bShadows && (frame.Flags & gpu::FrameFlag_SunEnabled) != 0u;
	XMMATRIX sunViewProj = XMMatrixIdentity();
	if (bSun)
	{
		const XMFLOAT3 lo = Scene.GetSceneBoundsMin();
		const XMFLOAT3 hi = Scene.GetSceneBoundsMax();
		const XMVECTOR toSun = XMVector3Normalize(XMLoadFloat3(&frame.SunDirection));
		const XMVECTOR up = std::abs(frame.SunDirection.y) > 0.99f ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f) : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
		const XMVECTOR center = XMVectorSet(0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z), 1.0f);
		const XMMATRIX view = XMMatrixLookToLH(center, XMVectorNegate(toSun), up);
		XMFLOAT3 lmin = { 1e30f, 1e30f, 1e30f };
		XMFLOAT3 lmax = { -1e30f, -1e30f, -1e30f };
		for (uint32 c = 0; c < 8u; ++c)
		{
			XMFLOAT3 p;
			XMStoreFloat3(&p, XMVector3TransformCoord(XMVectorSet((c & 1u) ? hi.x : lo.x, (c & 2u) ? hi.y : lo.y, (c & 4u) ? hi.z : lo.z, 1.0f), view));
			lmin = { (std::min)(lmin.x, p.x), (std::min)(lmin.y, p.y), (std::min)(lmin.z, p.z) };
			lmax = { (std::max)(lmax.x, p.x), (std::max)(lmax.y, p.y), (std::max)(lmax.z, p.z) };
		}
		// Two texels of margin keep the PCF footprint of boundary receivers inside the map.
		const float marginX = 2.0f * (lmax.x - lmin.x) / SunShadowSize + 1e-3f;
		const float marginY = 2.0f * (lmax.y - lmin.y) / SunShadowSize + 1e-3f;
		const float l = lmin.x - marginX, r = lmax.x + marginX;
		const float b = lmin.y - marginY, t = lmax.y + marginY;
		const float zn = lmin.z - 0.1f, zf = lmax.z + 0.1f;
		// Reversed-Z orthographic projection: z' = (zf - z) / (zf - zn).
		const XMMATRIX proj(
			2.0f / (r - l), 0.0f, 0.0f, 0.0f,
			0.0f, 2.0f / (t - b), 0.0f, 0.0f,
			0.0f, 0.0f, -1.0f / (zf - zn), 0.0f,
			-(r + l) / (r - l), -(t + b) / (t - b), zf / (zf - zn), 1.0f);
		sunViewProj = XMMatrixMultiply(view, proj);
		StoreView(Constants.Views[gpu::RasterSunShadowView], sunViewProj, 0.0f, 0.0f, 1.0f, 1.0f);
		Constants.SunShadowTexelWorld = (std::max)(r - l, t - b) / SunShadowSize;
		Constants.Flags |= gpu::RasterFlag_SunShadow;
	}

	// ---- cube shadows, one atlas row each: the emitters, then the portals of an enclosure ----
	// Reversed infinite perspective faces. An emitter's views start at its surface and leave
	// its own instance out; a portal's views look in through the opening and skip the face that
	// looks out (receivers on that side are outside the enclosure).
	struct SCubeShadow
	{
		XMFLOAT3 Center = {};
		float LightRadius = 0.0f;
		float NearZ = 0.01f;
		uint32 SkipInstance = FRT_INVALID_INDEX;
		uint32 SkipFace = 6u;
	};
	SCubeShadow cubes[gpu::RasterMaxCubeShadows];
	uint32 cubeCount = 0u;

	const std::vector<SRasterEmitter>& emitters = Scene.GetRasterEmitters();
	const uint32 emitterCount = (std::min)({ static_cast<uint32>(emitters.size()), gpu::RasterMaxEmitters, MaxEmitters });
	Constants.EmitterCount = emitterCount;
	for (uint32 e = 0; e < emitterCount; ++e)
	{
		const SRasterEmitter& src = emitters[e];
		gpu::SRasterEmitterData& dst = Constants.Emitters[e];
		dst.Position = src.Position;
		dst.Radius = src.Radius;
		dst.Radiance = src.Radiance;
		dst.ShadowView = FRT_INVALID_INDEX;
		if (bShadows && cubeCount < gpu::RasterMaxCubeShadows)
		{
			dst.ShadowView = 1u + 6u * cubeCount;
			SCubeShadow& cube = cubes[cubeCount++];
			cube.Center = src.Position;
			cube.LightRadius = src.Radius;
			cube.NearZ = (std::max)(src.Radius * 1.05f, 0.01f);
			cube.SkipInstance = src.InstanceIndex;
		}
	}
	const XMVECTOR faceDirs[6] = {
		XMVectorSet(1, 0, 0, 0), XMVectorSet(-1, 0, 0, 0), XMVectorSet(0, 1, 0, 0),
		XMVectorSet(0, -1, 0, 0), XMVectorSet(0, 0, 1, 0), XMVectorSet(0, 0, -1, 0) };
	const XMVECTOR faceUps[6] = {
		XMVectorSet(0, 1, 0, 0), XMVectorSet(0, 1, 0, 0), XMVectorSet(0, 0, -1, 0),
		XMVectorSet(0, 0, 1, 0), XMVectorSet(0, 1, 0, 0), XMVectorSet(0, 1, 0, 0) };
	Constants.PortalShadowViews = { FRT_INVALID_INDEX, FRT_INVALID_INDEX, FRT_INVALID_INDEX, FRT_INVALID_INDEX };
	for (uint32 p = 0; p < (std::min)(Constants.PortalCount, gpu::RasterMaxPortals); ++p)
	{
		if (!bShadows || cubeCount >= gpu::RasterMaxCubeShadows)
		{
			break;
		}
		const gpu::SPortalData& portal = Constants.Portals[p];
		(&Constants.PortalShadowViews.x)[p] = 1u + 6u * cubeCount;
		SCubeShadow& cube = cubes[cubeCount++];
		// A few centimetres OUTSIDE the opening, looking in: the interior is then seen through the
		// opening itself, so no sky gets in where the authored portal is not actually open (a
		// portal placed over a solid roof shadows everything below it).
		cube.Center = { portal.Center.x - portal.Normal.x * 0.05f, portal.Center.y - portal.Normal.y * 0.05f, portal.Center.z - portal.Normal.z * 0.05f };
		cube.LightRadius = std::sqrt(portal.Area / kPiF);
		cube.NearZ = 0.02f;
		float best = 0.5f;
		for (uint32 f = 0; f < 6u; ++f)
		{
			XMFLOAT3 d;
			XMStoreFloat3(&d, faceDirs[f]);
			const float outward = -(d.x * portal.Normal.x + d.y * portal.Normal.y + d.z * portal.Normal.z);
			if (outward > best)
			{
				best = outward;
				cube.SkipFace = f;
			}
		}
	}

	// A guard band of a few texels around each face keeps the PCF footprint inside the tile.
	const float scale = 1.0f / (1.0f + 2.0f * 3.0f / CubeTileSize);
	XMMATRIX faceViewProj[gpu::RasterMaxCubeShadows][6];
	for (uint32 c = 0; c < cubeCount; ++c)
	{
		const SCubeShadow& cube = cubes[c];
		const XMMATRIX proj(
			scale, 0.0f, 0.0f, 0.0f,
			0.0f, scale, 0.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f,
			0.0f, 0.0f, cube.NearZ, 0.0f);
		const XMVECTOR position = XMLoadFloat3(&cube.Center);
		for (uint32 f = 0; f < 6u; ++f)
		{
			faceViewProj[c][f] = XMMatrixMultiply(XMMatrixLookToLH(position, faceDirs[f], faceUps[f]), proj);
			gpu::SRasterShadowView& view = Constants.Views[1u + 6u * c + f];
			StoreView(view, faceViewProj[c][f],
				static_cast<float>(f * CubeTileSize) / LocalShadow.Width, static_cast<float>(c * CubeTileSize) / LocalShadow.Height,
				static_cast<float>(CubeTileSize) / LocalShadow.Width, static_cast<float>(CubeTileSize) / LocalShadow.Height);
			view.Origin = cube.Center;
			view.LightRadius = cube.LightRadius;
		}
	}

	// ---- render ----
	Context.Barriers.Transition(SunShadow, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	Context.Barriers.Transition(LocalShadow, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	Context.FlushBarriers();
	Context.BindGraphics();
	Context.PushGraphicsPassConstants(Constants);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	if (bSun)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE dsv = Device->GetDsvHeap().GetCpuHandle(SunShadow.Dsv);
		cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
		cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
		const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(SunShadowSize), static_cast<float>(SunShadowSize), 0.0f, 1.0f };
		RenderView(Context, Scene, gpu::RasterSunShadowView, viewport, sunViewProj, FRT_INVALID_INDEX);
	}
	if (cubeCount > 0u)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE dsv = Device->GetDsvHeap().GetCpuHandle(LocalShadow.Dsv);
		cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
		cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
		for (uint32 c = 0; c < cubeCount; ++c)
		{
			for (uint32 f = 0; f < 6u; ++f)
			{
				if (f == cubes[c].SkipFace)
				{
					continue;
				}
				const D3D12_VIEWPORT viewport = {
					static_cast<float>(f * CubeTileSize), static_cast<float>(c * CubeTileSize),
					static_cast<float>(CubeTileSize), static_cast<float>(CubeTileSize), 0.0f, 1.0f };
				RenderView(Context, Scene, 1u + 6u * c + f, viewport, faceViewProj[c][f], cubes[c].SkipInstance);
			}
		}
	}

	const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	Context.Barriers.Transition(SunShadow, read);
	Context.Barriers.Transition(LocalShadow, read);
	Context.FlushBarriers();
}
}
