#include "Graphics/Render/Passes/RasterPreview.h"

#include <algorithm>

#include "Graphics/Render/Scene/GpuScene.h"
#include "Profiler/Profiler.h"


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
	desc.bDepthWrite = true;
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
	for (rhi::SGpuTexture* t : { &Color, &Motion, &ViewZ, &Depth })
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
	for (rhi::SGpuTexture* t : { &Color, &Motion, &ViewZ, &Depth })
	{
		if (t->IsValid())
		{
			Device->Destroy(*t);
		}
	}
	Width = (std::max)(InWidth, 1u);
	Height = (std::max)(InHeight, 1u);

	auto makeRt = [&] (DXGI_FORMAT format, const wchar_t* name)
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
	Color = makeRt(DXGI_FORMAT_R16G16B16A16_FLOAT, L"Raster_Color");
	Motion = makeRt(DXGI_FORMAT_R16G16_FLOAT, L"Raster_Motion");
	ViewZ = makeRt(DXGI_FORMAT_R32_FLOAT, L"Raster_ViewZ");

	D3D12_CLEAR_VALUE depthClear = {};
	depthClear.Format = DXGI_FORMAT_D32_FLOAT;
	depthClear.DepthStencil.Depth = 0.0f;
	rhi::STextureDesc depth;
	depth.Width = Width;
	depth.Height = Height;
	depth.Format = DXGI_FORMAT_D32_FLOAT;
	depth.bDsv = true;
	depth.bSrv = false;
	depth.InitialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	depth.ClearValue = &depthClear;
	depth.Name = L"Raster_Depth";
	Depth = Device->CreateTexture(depth);
}

void CRasterPreview::Execute (CRenderContext& Context, const CGpuScene& Scene, float MipBias)
{
	FRT_GPU_SCOPE(Context.CommandList, "RasterPreview");
	ID3D12GraphicsCommandList4* cmd = Context.CommandList;

	Context.Barriers.Transition(Color, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Context.Barriers.Transition(Motion, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Context.Barriers.Transition(ViewZ, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Context.Barriers.Transition(Depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	Context.FlushBarriers();

	rhi::CCpuDescriptorHeap& rtvHeap = Device->GetRtvHeap();
	const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {
		rtvHeap.GetCpuHandle(Color.Rtv), rtvHeap.GetCpuHandle(Motion.Rtv), rtvHeap.GetCpuHandle(ViewZ.Rtv) };
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = Device->GetDsvHeap().GetCpuHandle(Depth.Dsv);
	const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	cmd->ClearRenderTargetView(rtvs[0], black, 0, nullptr);
	cmd->ClearRenderTargetView(rtvs[1], black, 0, nullptr);
	cmd->ClearRenderTargetView(rtvs[2], black, 0, nullptr);
	cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
	cmd->OMSetRenderTargets(3, rtvs, FALSE, &dsv);

	const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height), 0.0f, 1.0f };
	const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height) };
	cmd->RSSetViewports(1, &viewport);
	cmd->RSSetScissorRects(1, &scissor);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	Context.BindGraphics();

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
			if (draw.bAlphaBlend != (pass == 1))
			{
				continue;
			}
			uint32 constants[4] = { draw.InstanceIndex, draw.GeometryIndex, 0u, 0u };
			memcpy(&constants[2], &MipBias, sizeof(float));
			cmd->SetGraphicsRoot32BitConstants(rhi::RootParam_Constants, 4, constants, 0);
			cmd->IASetVertexBuffers(0, 1, &draw.VertexBuffer);
			cmd->IASetIndexBuffer(&draw.IndexBuffer);
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
}
