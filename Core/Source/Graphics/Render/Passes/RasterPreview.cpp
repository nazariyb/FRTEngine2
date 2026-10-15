#include "Graphics/Render/Passes/RasterPreview.h"

#include <algorithm>

#include "Graphics/Render/Scene/GpuScene.h"
#include "Profiler/Profiler.h"
#include "imgui.h"


namespace frt::graphics
{
namespace
{
std::vector<D3D12_INPUT_ELEMENT_DESC> MakeVertexLayout ()
{
	// Mirrors SVertex (GraphicsCoreTypes.h): position, uv, normal, tangent, bitangent, colour.
	return {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
}
}

void CRasterPreview::Initialize (rhi::CDevice& InDevice, rhi::CPipelineLibrary& InPipelines)
{
	Device = &InDevice;
	Pipelines = &InPipelines;
	Shadows.Initialize(InDevice, InPipelines);

	// Depth prepass: the same vertex shader as the shading pass (identical, precise positions),
	// so shading can test against this depth without writing it.
	rhi::SGraphicsPipelineDesc depthDesc;
	depthDesc.VertexShader = MakeShaderDesc("Raster/RasterPreview.hlsl", "VsMain", "vs_6_6");
	depthDesc.PixelShader = MakeShaderDesc("Raster/RasterPreview.hlsl", "PsDepth", "ps_6_6");
	depthDesc.InputLayout = MakeVertexLayout();
	depthDesc.RenderTargetCount = 0u;
	depthDesc.DepthFormat = DXGI_FORMAT_D32_FLOAT;
	depthDesc.bDepthTest = true;
	depthDesc.bDepthWrite = true;
	depthDesc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
	depthDesc.CullMode = D3D12_CULL_MODE_NONE;
	DepthPipeline = Pipelines->CreateGraphics("RasterPreview_Depth", depthDesc);
	depthDesc.PixelShader = MakeShaderDesc("Raster/RasterPreview.hlsl", "PsDepthMasked", "ps_6_6");
	DepthMaskedPipeline = Pipelines->CreateGraphics("RasterPreview_DepthMasked", depthDesc);
	AoPipeline = Pipelines->CreateCompute("RasterPreview_Ao", MakeShaderDesc("Raster/AmbientOcclusion.hlsl", "ComputeAo", "cs_6_6"));
	AoFilterPipeline = Pipelines->CreateCompute("RasterPreview_AoFilter", MakeShaderDesc("Raster/AmbientOcclusion.hlsl", "FilterAo", "cs_6_6"));

	rhi::SGraphicsPipelineDesc desc;
	desc.VertexShader = MakeShaderDesc("Raster/RasterPreview.hlsl", "VsMain", "vs_6_6");
	desc.PixelShader = MakeShaderDesc("Raster/RasterPreview.hlsl", "PsMain", "ps_6_6");
	desc.InputLayout = MakeVertexLayout();
	desc.RenderTargetCount = 3u;
	desc.RenderTargetFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.RenderTargetFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
	desc.RenderTargetFormats[2] = DXGI_FORMAT_R32_FLOAT;
	desc.DepthFormat = DXGI_FORMAT_D32_FLOAT;
	desc.bDepthTest = true;
	desc.bDepthWrite = false;   // the prepass wrote it
	desc.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
	desc.CullMode = D3D12_CULL_MODE_NONE;
	OpaquePipeline = Pipelines->CreateGraphics("RasterPreview_Opaque", desc);

	desc.bAlphaBlend = true;
	desc.bDepthWrite = false;
	BlendPipeline = Pipelines->CreateGraphics("RasterPreview_Blend", desc);

	rhi::SGraphicsPipelineDesc sky = desc;
	sky.VertexShader = MakeShaderDesc("Raster/RasterPreview.hlsl", "VsSky", "vs_6_6");
	sky.PixelShader = MakeShaderDesc("Raster/RasterPreview.hlsl", "PsSky", "ps_6_6");
	sky.InputLayout.clear();
	sky.bAlphaBlend = false;
	sky.bDepthWrite = false;
	sky.bDepthTest = true;
	SkyPipeline = Pipelines->CreateGraphics("RasterPreview_Sky", sky);
}

void CRasterPreview::Shutdown ()
{
	if (!Device)
	{
		return;
	}
	Shadows.Shutdown();
	for (rhi::SGpuTexture* t : { &Color, &Motion, &ViewZ, &Depth, &AoRaw, &Ao })
	{
		Device->Destroy(*t);
	}
	Device = nullptr;
}

void CRasterPreview::Resize (uint32 InWidth, uint32 InHeight)
{
	if (InWidth == Width && InHeight == Height && Color.IsValid())
	{
		return;
	}
	for (rhi::SGpuTexture* t : { &Color, &Motion, &ViewZ, &Depth, &AoRaw, &Ao })
	{
		if (t->IsValid())
		{
			Device->Destroy(*t);
		}
	}
	Width = (std::max)(InWidth, 1u);
	Height = (std::max)(InHeight, 1u);

	auto makeRt = [&] (DXGI_FORMAT format, FStringView name)
	{
		D3D12_CLEAR_VALUE clear = {};
		clear.Format = format;
		rhi::STextureDesc desc;
		desc.Width = Width;
		desc.Height = Height;
		desc.Format = format;
		desc.bRtv = true;
		desc.bSrv = true;
		desc.InitialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
		desc.ClearValue = &clear;
		desc.Name = name;
		return Device->CreateTexture(desc);
	};
	Color = makeRt(DXGI_FORMAT_R16G16B16A16_FLOAT, "Raster_Color");
	Motion = makeRt(DXGI_FORMAT_R16G16_FLOAT, "Raster_Motion");
	ViewZ = makeRt(DXGI_FORMAT_R32_FLOAT, "Raster_ViewZ");

	D3D12_CLEAR_VALUE depthClear = {};
	depthClear.Format = DXGI_FORMAT_D32_FLOAT;
	depthClear.DepthStencil.Depth = 0.0f;
	rhi::STextureDesc depth;
	depth.Width = Width;
	depth.Height = Height;
	depth.Format = DXGI_FORMAT_R32_TYPELESS;   // DSV + SRV (the AO reads the prepass depth)
	depth.DsvFormat = DXGI_FORMAT_D32_FLOAT;
	depth.SrvFormat = DXGI_FORMAT_R32_FLOAT;
	depth.bDsv = true;
	depth.bSrv = true;
	depth.InitialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	depth.ClearValue = &depthClear;
	depth.Name = "Raster_Depth";
	Depth = Device->CreateTexture(depth);

	rhi::STextureDesc ao;
	ao.Width = Width;
	ao.Height = Height;
	ao.Format = DXGI_FORMAT_R8_UNORM;
	ao.bUav = true;
	ao.InitialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	ao.Name = "Raster_AoRaw";
	AoRaw = Device->CreateTexture(ao);
	ao.Name = "Raster_Ao";
	Ao = Device->CreateTexture(ao);
}

void CRasterPreview::Execute (CRenderContext& Context, const CGpuScene& Scene, float MipBias)
{
	FRT_GPU_SCOPE(Context.CommandList, "RasterPreview");
	ID3D12GraphicsCommandList4* cmd = Context.CommandList;

	// Lighting inputs that need no ray tracing: sky maps, the enclosure's energy balance, the
	// emitter stand-ins and their shadow maps.
	gpu::SRasterConstants constants = {};
	constants.SkyPrefiltered = Scene.GetSkyPrefilteredSrv();
	constants.SkyIrradiance = Scene.GetSkyIrradianceSrv();
	constants.SkyPrefilterSlices = CGpuScene::SkyPrefilterSlices;
	constants.AmbientOcclusion = FRT_INVALID_INDEX;
	constants.DebugMode = Settings.DebugMode;
	const SRasterAmbient& ambient = Scene.GetRasterAmbient();
	if (ambient.bEnclosed)
	{
		constants.Flags |= gpu::RasterFlag_Enclosed;
		constants.AmbientRadiance = ambient.Radiance;
		constants.PortalCount = (std::min)(static_cast<uint32>(ambient.Portals.size()), gpu::RasterMaxPortals);
		for (uint32 i = 0; i < constants.PortalCount; ++i)
		{
			constants.Portals[i] = ambient.Portals[i];		}
	}
	Shadows.Execute(Context, Scene, Settings.bShadows, Settings.bEmitters ? gpu::RasterMaxEmitters : 0u, constants);

	const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height), 0.0f, 1.0f };
	const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height) };
	const DirectX::XMMATRIX viewProj = DirectX::XMLoadFloat4x4(&Context.Frame->ViewProj);
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = Device->GetDsvHeap().GetCpuHandle(Depth.Dsv);
	auto setDrawConstants = [&] (const SSceneDraw& Draw)
	{
		uint32 drawConstants[4] = { Draw.InstanceIndex, Draw.GeometryIndex, 0u, 0u };
		memcpy(&drawConstants[2], &MipBias, sizeof(float));
		cmd->SetGraphicsRoot32BitConstants(rhi::RootParam_Constants, 4, drawConstants, 0);
		cmd->IASetVertexBuffers(0, 1, &Draw.VertexBuffer);
		cmd->IASetIndexBuffer(&Draw.IndexBuffer);
	};

	// ---- depth prepass (opaque and alpha-tested surfaces) ----
	{
		FRT_GPU_SCOPE(cmd, "RasterPreview_Depth");
		Context.Barriers.Transition(Depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		Context.FlushBarriers();
		cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
		cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
		cmd->RSSetViewports(1, &viewport);
		cmd->RSSetScissorRects(1, &scissor);
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		Context.BindGraphics();
		Context.PushGraphicsPassConstants(constants);
		for (uint32 pass = 0; pass < 2u; ++pass)
		{
			ID3D12PipelineState* pso = Pipelines->Get(pass == 0u ? DepthPipeline : DepthMaskedPipeline);
			if (!pso)
			{
				continue;
			}
			cmd->SetPipelineState(pso);
			for (const SSceneDraw& draw : Scene.GetDraws())
			{
				if (draw.bAlphaBlend || draw.bAlphaTested != (pass == 1u) || IsBoxOutsideView(viewProj, draw.BoundsMin, draw.BoundsMax))
				{
					continue;
				}
				setDrawConstants(draw);
				cmd->DrawIndexedInstanced(draw.IndexCount, 1, draw.IndexOffset, draw.VertexOffset, 0);
			}
		}
		Context.Barriers.Transition(Depth, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		Context.FlushBarriers();
	}

	// ---- ambient occlusion from the prepass depth ----
	if (Settings.bAmbientOcclusion && Pipelines->Get(AoPipeline) && Pipelines->Get(AoFilterPipeline))
	{
		FRT_GPU_SCOPE(cmd, "RasterPreview_Ao");
		const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		Context.BindCompute();
		uint32 ao[8] = { Depth.Srv, FRT_INVALID_INDEX, AoRaw.Uav, 0u, Width, Height, 0u, 0u };
		const float radius = (std::max)(Settings.AmbientOcclusionRadius, 0.01f);
		memcpy(&ao[3], &radius, sizeof(float));
		Context.Barriers.Transition(AoRaw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Context.SetRootConstants(ao, 8u);
		Context.Dispatch2D(AoPipeline, Width, Height);
		ao[1] = AoRaw.Srv;
		ao[2] = Ao.Uav;
		Context.Barriers.Transition(AoRaw, read);
		Context.Barriers.Transition(Ao, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Context.SetRootConstants(ao, 8u);
		Context.Dispatch2D(AoFilterPipeline, Width, Height);
		Context.Barriers.Transition(Ao, read);
		Context.FlushBarriers();
		constants.Flags |= gpu::RasterFlag_AmbientOcclusion;
		constants.AmbientOcclusion = Ao.Srv;
	}

	// ---- shading against the prepass depth, then sky, then blended surfaces ----
	Context.Barriers.Transition(Color, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Context.Barriers.Transition(Motion, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Context.Barriers.Transition(ViewZ, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Context.FlushBarriers();

	rhi::CCpuDescriptorHeap& rtvHeap = Device->GetRtvHeap();
	const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {
		rtvHeap.GetCpuHandle(Color.Rtv), rtvHeap.GetCpuHandle(Motion.Rtv), rtvHeap.GetCpuHandle(ViewZ.Rtv) };
	const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	cmd->ClearRenderTargetView(rtvs[0], black, 0, nullptr);
	cmd->ClearRenderTargetView(rtvs[1], black, 0, nullptr);
	cmd->ClearRenderTargetView(rtvs[2], black, 0, nullptr);
	cmd->OMSetRenderTargets(3, rtvs, FALSE, &dsv);
	cmd->RSSetViewports(1, &viewport);
	cmd->RSSetScissorRects(1, &scissor);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	Context.BindGraphics();
	Context.PushGraphicsPassConstants(constants);

	for (uint32 pass = 0; pass < 2; ++pass)
	{
		ID3D12PipelineState* pso = Pipelines->Get(pass == 0 ? OpaquePipeline : BlendPipeline);
		if (!pso)
		{
			continue;
		}
		cmd->SetPipelineState(pso);
		for (const SSceneDraw& draw : Scene.GetDraws())
		{
			if (draw.bAlphaBlend != (pass == 1) || IsBoxOutsideView(viewProj, draw.BoundsMin, draw.BoundsMax))
			{
				continue;
			}
			setDrawConstants(draw);
			cmd->DrawIndexedInstanced(draw.IndexCount, 1, draw.IndexOffset, draw.VertexOffset, 0);
		}
		if (pass == 0)
		{
			// Sky only where no geometry wrote depth (reversed Z: cleared to 0 = far).
			if (ID3D12PipelineState* skyPso = Pipelines->Get(SkyPipeline))
			{
				cmd->SetPipelineState(skyPso);
				cmd->DrawInstanced(3, 1, 0, 0);
			}
		}
	}

	const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	Context.Barriers.Transition(Color, read);
	Context.Barriers.Transition(Motion, read);
	Context.Barriers.Transition(ViewZ, read);
	Context.FlushBarriers();
}

void CRasterPreview::DrawSettingsUi ()
{
	ImGui::Checkbox("Shadow maps", &Settings.bShadows);
	ImGui::Checkbox("Emitters as sphere lights", &Settings.bEmitters);
	ImGui::Checkbox("Ambient occlusion (GTAO)", &Settings.bAmbientOcclusion);
	ImGui::SliderFloat("AO radius", &Settings.AmbientOcclusionRadius, 0.1f, 4.0f, "%.2f m");
	int debug = static_cast<int>(Settings.DebugMode);
	const char* modes[] = { "Off", "Sun", "Emitters", "Sky", "Ambient (energy balance)", "Specular", "Diffuse", "Sun shadow", "Occlusion", "Normals" };
	if (ImGui::Combo("Debug view", &debug, modes, IM_ARRAYSIZE(modes)))
	{
		Settings.DebugMode = static_cast<uint32>(debug);
	}
}
}
