#include "Graphics/Render/Passes/PathTracerPrograms.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>


namespace frt::graphics
{
namespace
{
constexpr wchar_t kRayGen[] = L"RayGen";
constexpr wchar_t kMiss[] = L"Miss";
constexpr wchar_t kClosestHit[] = L"ClosestHit";
constexpr wchar_t kAnyHit[] = L"AnyHit";
constexpr wchar_t kHitGroup[] = L"HitGroup";

// Payload: 5 dwords out + barycentrics; see SHitPayload in PathTracer.hlsl.
constexpr uint32 kMaxPayloadBytes = 32u;
constexpr uint32 kMaxAttributeBytes = 8u;
}


void CPathTracerPrograms::Initialize (rhi::CDevice& InDevice, rhi::CPipelineLibrary& InPipelines)
{
	Device = &InDevice;
	Pipelines = &InPipelines;

	rhi::SShaderCompileDesc base = MakeShaderDesc("PathTracing/PathTracer.hlsl", "", "lib_6_8", { { "FRT_SER", "0" } });
	Base.Library = Pipelines->RegisterLibrary("PathTracer", base);

	if (Device->GetCaps().bShaderExecutionReordering)
	{
		rhi::SShaderCompileDesc ser = MakeShaderDesc("PathTracing/PathTracer.hlsl", "", "lib_6_9", { { "FRT_SER", "1" } });
		// With payload access qualifiers the lib_6_9 state object never makes closest-hit writes
		// visible to the caller (plain TraceRay returns the caller's initial payload; observed on
		// RTX 40 / Agility 1.619). The payload is tiny, so qualifiers buy nothing here anyway.
		ser.ExtraArguments.push_back(L"-disable-payload-qualifiers");
		ser.ExtraArguments.push_back(L"-Wno-payload-access-qualifier"); // the [raypayload] annotations are intentionally ignored here
		Ser.Library = Pipelines->RegisterLibrary("PathTracer(SER)", ser);
		Ser.bReorder = true;
	}

	Remodulate = Pipelines->CreateCompute("PathTracer_Remodulate", MakeShaderDesc("PathTracing/Remodulate.hlsl", "main", "cs_6_6"));
}

void CPathTracerPrograms::Shutdown ()
{
	if (!Device)
	{
		return;
	}
	for (SVariant* v : { &Ser, &Base })
	{
		if (v->StateObject)
		{
			Device->DeferRelease(rhi::ComPtr<IUnknown>(v->StateObject));
			v->StateObject.Reset();
		}
		Device->Destroy(v->ShaderTable);
		v->bValid = false;
	}
	Device = nullptr;
}

bool CPathTracerPrograms::Build (SVariant& Variant, const wchar_t* Name, ID3D12GraphicsCommandList* CommandList)
{
	const rhi::SShaderBinary* library = Pipelines->GetLibrary(Variant.Library);
	if (!library)
	{
		return false;
	}
	ID3D12Device5* device = Device->GetDevice();

	D3D12_EXPORT_DESC exports[] = {
		{ kRayGen, nullptr, D3D12_EXPORT_FLAG_NONE },
		{ kMiss, nullptr, D3D12_EXPORT_FLAG_NONE },
		{ kClosestHit, nullptr, D3D12_EXPORT_FLAG_NONE },
		{ kAnyHit, nullptr, D3D12_EXPORT_FLAG_NONE },
	};
	D3D12_DXIL_LIBRARY_DESC libDesc = {};
	libDesc.DXILLibrary = library->GetBytecode();
	libDesc.NumExports = static_cast<UINT>(std::size(exports));
	libDesc.pExports = exports;

	D3D12_HIT_GROUP_DESC hitGroup = {};
	hitGroup.HitGroupExport = kHitGroup;
	hitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
	hitGroup.ClosestHitShaderImport = kClosestHit;
	hitGroup.AnyHitShaderImport = kAnyHit;

	D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
	shaderConfig.MaxPayloadSizeInBytes = kMaxPayloadBytes;
	shaderConfig.MaxAttributeSizeInBytes = kMaxAttributeBytes;

	D3D12_GLOBAL_ROOT_SIGNATURE globalRoot = {};
	globalRoot.pGlobalRootSignature = Pipelines->GetComputeRootSignature();

	// Every bounce is traced from ray generation: recursion depth 1. Shadow rays are inline.
	D3D12_RAYTRACING_PIPELINE_CONFIG1 pipelineConfig = {};
	pipelineConfig.MaxTraceRecursionDepth = 1u;
	pipelineConfig.Flags = D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

	D3D12_STATE_SUBOBJECT subobjects[5] = {};
	subobjects[0] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc };
	subobjects[1] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroup };
	subobjects[2] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig };
	subobjects[3] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRoot };
	subobjects[4] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1, &pipelineConfig };

	D3D12_STATE_OBJECT_DESC desc = {};
	desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
	desc.NumSubobjects = static_cast<UINT>(std::size(subobjects));
	desc.pSubobjects = subobjects;

	rhi::ComPtr<ID3D12StateObject> stateObject;
	const HRESULT hr = device->CreateStateObject(&desc, IID_PPV_ARGS(&stateObject));
	if (FAILED(hr))
	{
		std::fprintf(stderr, "[pathtracer] CreateStateObject failed for %ls (0x%08lx)\n", Name, hr);
		return false;
	}
	rhi::SetDebugName(stateObject.Get(), Name);

	rhi::ComPtr<ID3D12StateObjectProperties> props;
	THROW_IF_FAILED(stateObject.As(&props));

	// Exact stack: raygen + the deepest of what one TraceRay can run (closest hit, any hit or
	// miss). The runtime default assumes the worst for every shader, which wastes memory.
	const UINT64 rgs = props->GetShaderStackSize(kRayGen);
	const UINT64 chs = props->GetShaderStackSize(L"HitGroup::closesthit");
	const UINT64 ahs = props->GetShaderStackSize(L"HitGroup::anyhit");
	const UINT64 ms = props->GetShaderStackSize(kMiss);
	props->SetPipelineStackSize(rgs + (std::max)({ chs, ahs, ms }));

	// Shader table: raygen | miss | hit group, one record each, no local arguments.
	const uint32 recordSize = static_cast<uint32>(rhi::AlignUp(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT));
	const uint32 tableStride = static_cast<uint32>(rhi::AlignUp(recordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT));
	std::vector<uint8> table(tableStride * 3u, 0u);
	memcpy(table.data() + 0u * tableStride, props->GetShaderIdentifier(kRayGen), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
	memcpy(table.data() + 1u * tableStride, props->GetShaderIdentifier(kMiss), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
	memcpy(table.data() + 2u * tableStride, props->GetShaderIdentifier(kHitGroup), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

	rhi::SBufferDesc tableDesc;
	tableDesc.Size = table.size();
	tableDesc.Name = L"PathTracerShaderTable";
	rhi::SGpuBuffer shaderTable = Device->CreateBuffer(tableDesc);
	Device->UploadBuffer(CommandList, shaderTable, table.data(), table.size(), 0u, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	if (Variant.StateObject)
	{
		Device->DeferRelease(rhi::ComPtr<IUnknown>(Variant.StateObject));
	}
	Device->Destroy(Variant.ShaderTable);
	Variant.StateObject = stateObject;
	Variant.ShaderTable = std::move(shaderTable);

	const D3D12_GPU_VIRTUAL_ADDRESS base = Variant.ShaderTable.GetGpuAddress();
	D3D12_DISPATCH_RAYS_DESC& d = Variant.DispatchTemplate;
	d = {};
	d.RayGenerationShaderRecord = { base, recordSize };
	d.MissShaderTable = { base + tableStride, recordSize, recordSize };
	d.HitGroupTable = { base + 2u * tableStride, recordSize, recordSize };
	d.Depth = 1u;

	Variant.bValid = true;
	return true;
}

const CPathTracerPrograms::SVariant* CPathTracerPrograms::Select (bool bWantSer) const
{
	if (bWantSer && Ser.bValid)
	{
		return &Ser;
	}
	return Base.bValid ? &Base : nullptr;
}

void CPathTracerPrograms::Update (ID3D12GraphicsCommandList* CommandList)
{
	if (!Device)
	{
		return;
	}
	for (SVariant* variant : { &Base, &Ser })
	{
		if (variant->Library == rhi::InvalidPipelineHandle)
		{
			continue;
		}
		const uint32 version = Pipelines->GetLibraryVersion(variant->Library);
		if (version != variant->BuiltVersion && Pipelines->GetLibrary(variant->Library))
		{
			variant->BuiltVersion = version;
			Build(*variant, variant->bReorder ? L"PathTracer(SER)" : L"PathTracer", CommandList);
		}
	}
}
}
