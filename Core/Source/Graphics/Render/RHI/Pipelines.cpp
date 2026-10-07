#include "Graphics/Render/RHI/Pipelines.h"

#include <cstdio>

#include "Graphics/Render/RHI/Device.h"


namespace frt::graphics::rhi
{
namespace
{
D3D12_STATIC_SAMPLER_DESC MakeSampler (
	uint32 Register,
	D3D12_FILTER Filter,
	D3D12_TEXTURE_ADDRESS_MODE Address,
	uint32 Anisotropy = 1u)
{
	D3D12_STATIC_SAMPLER_DESC desc = {};
	desc.Filter = Filter;
	desc.AddressU = Address;
	desc.AddressV = Address;
	desc.AddressW = Address;
	desc.MipLODBias = 0.0f;
	desc.MaxAnisotropy = Anisotropy;
	desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	desc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	desc.MinLOD = 0.0f;
	desc.MaxLOD = D3D12_FLOAT32_MAX;
	desc.ShaderRegister = Register;
	desc.RegisterSpace = 0u;
	desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	return desc;
}

ComPtr<ID3D12RootSignature> CreateGlobalRootSignature (ID3D12Device* Device, bool bInputAssembler)
{
	D3D12_ROOT_PARAMETER1 params[RootParam_Count] = {};

	params[RootParam_Constants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[RootParam_Constants].Constants.ShaderRegister = 0u;
	params[RootParam_Constants].Constants.RegisterSpace = 0u;
	params[RootParam_Constants].Constants.Num32BitValues = RootConstantCount;
	params[RootParam_Constants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	params[RootParam_FrameConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[RootParam_FrameConstants].Descriptor.ShaderRegister = 1u;
	params[RootParam_FrameConstants].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
	params[RootParam_FrameConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	params[RootParam_PassConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[RootParam_PassConstants].Descriptor.ShaderRegister = 2u;
	params[RootParam_PassConstants].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
	params[RootParam_PassConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	params[RootParam_SceneTlas].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
	params[RootParam_SceneTlas].Descriptor.ShaderRegister = 0u;
	params[RootParam_SceneTlas].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
	params[RootParam_SceneTlas].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	const D3D12_STATIC_SAMPLER_DESC samplers[] = {
		MakeSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
		MakeSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
		MakeSampler(2, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
		MakeSampler(3, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
		MakeSampler(4, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 16u),
		MakeSampler(5, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_BORDER),
	};

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
	desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = RootParam_Count;
	desc.Desc_1_1.pParameters = params;
	desc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
	desc.Desc_1_1.pStaticSamplers = samplers;
	desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
	if (bInputAssembler)
	{
		desc.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	}

	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> error;
	const HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
	if (FAILED(hr))
	{
		if (error)
		{
			std::fprintf(stderr, "[rhi] root signature: %s\n", static_cast<const char*>(error->GetBufferPointer()));
		}
		THROW_IF_FAILED(hr);
	}

	ComPtr<ID3D12RootSignature> rootSignature;
	THROW_IF_FAILED(Device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));
	SetDebugName(rootSignature.Get(), bInputAssembler ? L"GlobalRootSignature(Graphics)" : L"GlobalRootSignature(Compute)");
	return rootSignature;
}
}


void CPipelineLibrary::Init (CDevice& InDevice, CShaderCompiler& InCompiler)
{
	Device = &InDevice;
	Compiler = &InCompiler;
	ComputeRootSignature = CreateGlobalRootSignature(Device->GetDevice(), false);
	GraphicsRootSignature = CreateGlobalRootSignature(Device->GetDevice(), true);
}

void CPipelineLibrary::Shutdown ()
{
	Programs.clear();
	ComputeRootSignature.Reset();
	GraphicsRootSignature.Reset();
}

HPipeline CPipelineLibrary::CreateCompute (std::string_view Name, const SShaderCompileDesc& Shader)
{
	SProgram program;
	program.Name = std::string(Name);
	program.Kind = EKind::Compute;
	program.Shader = Shader;
	Build(program);
	Programs.push_back(std::move(program));
	return static_cast<HPipeline>(Programs.size() - 1u);
}

HPipeline CPipelineLibrary::CreateGraphics (std::string_view Name, const SGraphicsPipelineDesc& Desc)
{
	SProgram program;
	program.Name = std::string(Name);
	program.Kind = EKind::Graphics;
	program.Graphics = Desc;
	Build(program);
	Programs.push_back(std::move(program));
	return static_cast<HPipeline>(Programs.size() - 1u);
}

HShaderLibrary CPipelineLibrary::RegisterLibrary (std::string_view Name, const SShaderCompileDesc& Desc)
{
	SProgram program;
	program.Name = std::string(Name);
	program.Kind = EKind::Library;
	program.Shader = Desc;
	Build(program);
	Programs.push_back(std::move(program));
	return static_cast<HShaderLibrary>(Programs.size() - 1u);
}

ID3D12PipelineState* CPipelineLibrary::Get (HPipeline Handle) const
{
	return Handle < Programs.size() ? Programs[Handle].Pso.Get() : nullptr;
}

const SShaderBinary* CPipelineLibrary::GetLibrary (HShaderLibrary Handle) const
{
	if (Handle >= Programs.size() || !Programs[Handle].Library.bSuccess)
	{
		return nullptr;
	}
	return &Programs[Handle].Library;
}

uint32 CPipelineLibrary::GetLibraryVersion (HShaderLibrary Handle) const
{
	return Handle < Programs.size() ? Programs[Handle].Version : 0u;
}

void CPipelineLibrary::Watch (SProgram& Program, const std::vector<std::filesystem::path>& Dependencies)
{
	for (const std::filesystem::path& dep : Dependencies)
	{
		bool bKnown = false;
		for (const SWatchedFile& file : Program.Watched)
		{
			if (file.Path == dep)
			{
				bKnown = true;
				break;
			}
		}
		if (!bKnown)
		{
			std::error_code ec;
			Program.Watched.push_back({ dep, std::filesystem::last_write_time(dep, ec) });
		}
	}
}

bool CPipelineLibrary::IsStale (const SProgram& Program) const
{
	for (const SWatchedFile& file : Program.Watched)
	{
		std::error_code ec;
		const auto writeTime = std::filesystem::last_write_time(file.Path, ec);
		if (!ec && writeTime != file.WriteTime)
		{
			return true;
		}
	}
	return false;
}

bool CPipelineLibrary::Build (SProgram& Program)
{
	ID3D12Device5* device = Device->GetDevice();

	// Refresh the watch list's timestamps up front: a failed build must not be retried every
	// frame, only after the next edit.
	for (SWatchedFile& file : Program.Watched)
	{
		std::error_code ec;
		file.WriteTime = std::filesystem::last_write_time(file.Path, ec);
	}

	if (Program.Kind == EKind::Library)
	{
		SShaderBinary binary = Compiler->Compile(Program.Shader);
		Watch(Program, binary.Dependencies);
		if (!binary.bSuccess)
		{
			Errors.push_back(Program.Name + ":\n" + binary.Log);
			return false;
		}
		Program.Library = std::move(binary);
		++Program.Version;
		return true;
	}

	if (Program.Kind == EKind::Compute)
	{
		SShaderBinary cs = Compiler->Compile(Program.Shader);
		Watch(Program, cs.Dependencies);
		if (!cs.bSuccess)
		{
			Errors.push_back(Program.Name + ":\n" + cs.Log);
			return false;
		}

		D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = ComputeRootSignature.Get();
		desc.CS = cs.GetBytecode();
		ComPtr<ID3D12PipelineState> pso;
		const HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
		if (FAILED(hr))
		{
			Errors.push_back(Program.Name + ": CreateComputePipelineState failed");
			std::fprintf(stderr, "[rhi] CreateComputePipelineState failed for %s (0x%08lx)\n", Program.Name.c_str(), hr);
			return false;
		}
		SetDebugName(pso.Get(), Widen(Program.Name));
		if (Program.Pso)
		{
			Device->DeferRelease(ComPtr<IUnknown>(Program.Pso));
		}
		Program.Pso = std::move(pso);
		++Program.Version;
		return true;
	}

	// Graphics
	const SGraphicsPipelineDesc& g = Program.Graphics;
	SShaderBinary vs = Compiler->Compile(g.VertexShader);
	SShaderBinary ps = Compiler->Compile(g.PixelShader);
	Watch(Program, vs.Dependencies);
	Watch(Program, ps.Dependencies);
	if (!vs.bSuccess || !ps.bSuccess)
	{
		Errors.push_back(Program.Name + ":\n" + vs.Log + ps.Log);
		return false;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	desc.pRootSignature = GraphicsRootSignature.Get();
	desc.VS = vs.GetBytecode();
	desc.PS = ps.GetBytecode();
	desc.BlendState.AlphaToCoverageEnable = FALSE;
	desc.BlendState.IndependentBlendEnable = FALSE;
	for (uint32 i = 0; i < 8; ++i)
	{
		D3D12_RENDER_TARGET_BLEND_DESC& rt = desc.BlendState.RenderTarget[i];
		rt.BlendEnable = g.bAlphaBlend ? TRUE : FALSE;
		rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		rt.BlendOp = D3D12_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D12_BLEND_ONE;
		rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	}
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = g.CullMode;
	desc.RasterizerState.FrontCounterClockwise = FALSE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.DepthStencilState.DepthEnable = g.bDepthTest ? TRUE : FALSE;
	desc.DepthStencilState.DepthWriteMask = g.bDepthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = g.DepthFunc;
	desc.InputLayout.pInputElementDescs = g.InputLayout.empty() ? nullptr : g.InputLayout.data();
	desc.InputLayout.NumElements = static_cast<UINT>(g.InputLayout.size());
	desc.PrimitiveTopologyType = g.Topology;
	desc.NumRenderTargets = g.RenderTargetCount;
	for (uint32 i = 0; i < g.RenderTargetCount; ++i)
	{
		desc.RTVFormats[i] = g.RenderTargetFormats[i];
	}
	desc.DSVFormat = g.DepthFormat;
	desc.SampleDesc.Count = 1;

	ComPtr<ID3D12PipelineState> pso;
	const HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
	if (FAILED(hr))
	{
		Errors.push_back(Program.Name + ": CreateGraphicsPipelineState failed");
		std::fprintf(stderr, "[rhi] CreateGraphicsPipelineState failed for %s (0x%08lx)\n", Program.Name.c_str(), hr);
		return false;
	}
	SetDebugName(pso.Get(), Widen(Program.Name));
	if (Program.Pso)
	{
		Device->DeferRelease(ComPtr<IUnknown>(Program.Pso));
	}
	Program.Pso = std::move(pso);
	++Program.Version;
	return true;
}

bool CPipelineLibrary::PollHotReload ()
{
	if (!bHotReload)
	{
		return false;
	}
	// Stat-ing every dependency each frame is cheap but pointless; ~4 Hz is plenty.
	if ((++PollCounter % 15u) != 0u)
	{
		return false;
	}

	bool bAnyRebuilt = false;
	for (SProgram& program : Programs)
	{
		if (!IsStale(program))
		{
			continue;
		}
		std::fprintf(stdout, "[shader] hot reload: %s\n", program.Name.c_str());
		if (Build(program))
		{
			bAnyRebuilt = true;
		}
	}
	if (bAnyRebuilt)
	{
		Errors.clear();
	}
	return bAnyRebuilt;
}

void BindComputeRoot (
	ID3D12GraphicsCommandList* CommandList,
	CPipelineLibrary& Pipelines,
	CDevice& Device,
	D3D12_GPU_VIRTUAL_ADDRESS FrameConstants,
	D3D12_GPU_VIRTUAL_ADDRESS SceneTlas)
{
	ID3D12DescriptorHeap* heaps[] = { Device.GetBindlessHeap().GetHeap() };
	CommandList->SetDescriptorHeaps(1, heaps);
	CommandList->SetComputeRootSignature(Pipelines.GetComputeRootSignature());
	CommandList->SetComputeRootConstantBufferView(RootParam_FrameConstants, FrameConstants);
	// The pass slot must always hold a valid address; passes overwrite it.
	CommandList->SetComputeRootConstantBufferView(RootParam_PassConstants, FrameConstants);
	if (SceneTlas)
	{
		CommandList->SetComputeRootShaderResourceView(RootParam_SceneTlas, SceneTlas);
	}
}

void BindGraphicsRoot (
	ID3D12GraphicsCommandList* CommandList,
	CPipelineLibrary& Pipelines,
	CDevice& Device,
	D3D12_GPU_VIRTUAL_ADDRESS FrameConstants,
	D3D12_GPU_VIRTUAL_ADDRESS SceneTlas)
{
	ID3D12DescriptorHeap* heaps[] = { Device.GetBindlessHeap().GetHeap() };
	CommandList->SetDescriptorHeaps(1, heaps);
	CommandList->SetGraphicsRootSignature(Pipelines.GetGraphicsRootSignature());
	CommandList->SetGraphicsRootConstantBufferView(RootParam_FrameConstants, FrameConstants);
	CommandList->SetGraphicsRootConstantBufferView(RootParam_PassConstants, FrameConstants);
	if (SceneTlas)
	{
		CommandList->SetGraphicsRootShaderResourceView(RootParam_SceneTlas, SceneTlas);
	}
}
}
