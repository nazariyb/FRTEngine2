#include "DXRUtils.h"

#include <cstring>
#include <dxcapi.h>
#include <stdexcept>
#include <unordered_set>

#include "Exception.h"


namespace frt::graphics::raytracing
{
void CBottomLevelASGenerator::AddVertexBuffer (
	ID3D12Resource* VertexBuffer,
	uint64 VertexBufferOffset,
	uint32 VertexCount,
	uint32 VertexSize,
	ID3D12Resource* IndexBuffer,
	uint64 IndexBufferOffset,
	uint32 IndexCount,
	ID3D12Resource* TransformBuffer,
	uint64 TransformBufferOffset,
	bool bOpaque)
{
	VertexBuffers.Add(
		D3D12_RAYTRACING_GEOMETRY_DESC
		{
			.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
			.Flags = bOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE,
			.Triangles = D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC
			{
				.Transform3x4 = TransformBuffer ? TransformBuffer->GetGPUVirtualAddress() + TransformBufferOffset : 0,
				.IndexFormat = DXGI_FORMAT_R32_UINT,
				.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
				.IndexCount = IndexCount,
				.VertexCount = VertexCount,
				.IndexBuffer = IndexBuffer->GetGPUVirtualAddress() + IndexBufferOffset,
				.VertexBuffer = D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE
				{
					.StartAddress = VertexBuffer->GetGPUVirtualAddress() + VertexBufferOffset,
					.StrideInBytes = VertexSize
				}
			}
		});
}

void CBottomLevelASGenerator::ComputeASBufferSizes (
	ID3D12Device5* Device,
	bool bAllowUpdate,
	uint64* OutScratchSizeInBytes,
	uint64* OutResultSizeInBytes)
{
	Flags = bAllowUpdate
				? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
				: D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

	const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ASinputs = {
		.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
		.Flags = Flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
		.NumDescs = VertexBuffers.Count(),
		.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
		.pGeometryDescs = VertexBuffers.GetData()
	};

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ASinfo = {};
	Device->GetRaytracingAccelerationStructurePrebuildInfo(&ASinputs, &ASinfo);

	*OutScratchSizeInBytes = ScratchSizeInBytes =
							memory::AlignAddress(
								ASinfo.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
	*OutResultSizeInBytes = ResultSizeInBytes =
							memory::AlignAddress(
								ASinfo.ResultDataMaxSizeInBytes,
								D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
}

void CBottomLevelASGenerator::Generate (
	ID3D12GraphicsCommandList4* CommandList,
	ID3D12Resource* ScratchBuffer,
	ID3D12Resource* ResultBuffer,
	bool bUpdateOnly,
	ID3D12Resource* PreviousResult)
{
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags = Flags;
	if (flags == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE && bUpdateOnly)
	{
		flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
	}

	frt_assert(Flags == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE || !bUpdateOnly);
	frt_assert(!bUpdateOnly || PreviousResult != nullptr);
	frt_assert(ResultSizeInBytes > 0 && ScratchSizeInBytes > 0);

	const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc
	{
		.DestAccelerationStructureData = ResultBuffer->GetGPUVirtualAddress(),
		.Inputs = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
		{
			.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
			.Flags = flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
			.NumDescs = VertexBuffers.Count(),
			.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
			.pGeometryDescs = VertexBuffers.GetData()
		},
		.SourceAccelerationStructureData = PreviousResult ? PreviousResult->GetGPUVirtualAddress() : 0,
		.ScratchAccelerationStructureData = ScratchBuffer->GetGPUVirtualAddress()
	};

	CommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	const D3D12_RESOURCE_BARRIER uavBarrier
	{
		.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
		.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
		.UAV = ResultBuffer
	};
	CommandList->ResourceBarrier(1, &uavBarrier);
}


void CTopLevelASGenerator::AddInstance (
	ID3D12Resource* BottomLevelAS,
	const DirectX::XMFLOAT3X4& Transform,
	uint32 InstanceID,
	uint32 HitGroupIndex)
{
	Instances.Add(
		SInstance
		{
			.BottomLevelAS = BottomLevelAS,
			.Transform = Transform,
			.InstanceID = InstanceID,
			.HitGroupIndex = HitGroupIndex
		});
}

void CTopLevelASGenerator::ComputeASBufferSizes (
	ID3D12Device5* Device,
	bool bAllowUpdate,
	uint64* OutScratchSizeInBytes,
	uint64* OutResultSizeInBytes,
	uint64* OutDescriptorSizeInBytes)
{
	Flags = bAllowUpdate
				? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
				: D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

	const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ASinputs
	{
		.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
		.Flags = Flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
		.NumDescs = Instances.Count(),
		.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
	};

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
	Device->GetRaytracingAccelerationStructurePrebuildInfo(&ASinputs, &info);

	*OutScratchSizeInBytes = ScratchSizeInBytes =
							memory::AlignAddress(
								info.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
	*OutResultSizeInBytes = ResultSizeInBytes =
							memory::AlignAddress(
								info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
	*OutDescriptorSizeInBytes = DescriptorSizeInBytes =
								memory::AlignAddress(
									static_cast<uint64>(Instances.Count()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
									D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
}

void CTopLevelASGenerator::Generate (
	ID3D12GraphicsCommandList4* CommandList,
	ID3D12Resource* ScratchBuffer,
	ID3D12Resource* ResultBuffer,
	ID3D12Resource* DescriptorsBuffer,
	bool bUpdateOnly,
	ID3D12Resource* PreviousResult)
{
	D3D12_RAYTRACING_INSTANCE_DESC* instanceDescs = nullptr;
	const bool bHasInstances = Instances.Count() > 0u;
	if (bHasInstances)
	{
		frt_assert(DescriptorsBuffer != nullptr);
		const HRESULT mapResult = DescriptorsBuffer->Map(0, nullptr, reinterpret_cast<void**>(&instanceDescs));
		frt_assert(SUCCEEDED(mapResult) && instanceDescs);

		memset(instanceDescs, 0, DescriptorSizeInBytes);

		for (uint32 i = 0; i < Instances.Count(); ++i)
		{
			instanceDescs[i].InstanceID = Instances[i].InstanceID;
			instanceDescs[i].InstanceMask = 0xFF;
			instanceDescs[i].InstanceContributionToHitGroupIndex = Instances[i].HitGroupIndex;
			instanceDescs[i].AccelerationStructure = Instances[i].BottomLevelAS->GetGPUVirtualAddress();
			instanceDescs[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
			memcpy(instanceDescs[i].Transform, &Instances[i].Transform, sizeof(Instances[i].Transform));
		}

		DescriptorsBuffer->Unmap(0, nullptr);
	}

	D3D12_GPU_VIRTUAL_ADDRESS sourceAS = bUpdateOnly ? PreviousResult->GetGPUVirtualAddress() : 0;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags = Flags;
	if (flags == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE && bUpdateOnly)
	{
		flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
	}

	frt_assert(Flags == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE || !bUpdateOnly);
	frt_assert(!bUpdateOnly || PreviousResult != nullptr);

	const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc
	{
		.DestAccelerationStructureData = ResultBuffer->GetGPUVirtualAddress(),
		.Inputs = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
		{
			.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
			.Flags = flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
			.NumDescs = Instances.Count(),
			.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
			.InstanceDescs = bHasInstances ? DescriptorsBuffer->GetGPUVirtualAddress() : 0,
		},
		.SourceAccelerationStructureData = sourceAS,
		.ScratchAccelerationStructureData = ScratchBuffer->GetGPUVirtualAddress(),
	};

	CommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	const D3D12_RESOURCE_BARRIER uavBarrier
	{
		.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
		.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
		.UAV = ResultBuffer
	};
	CommandList->ResourceBarrier(1, &uavBarrier);
}

CRayTracingPipelineGenerator::CRayTracingPipelineGenerator (ID3D12Device5* InDevice)
	: Device(InDevice)
{
	CreateDummyRootSignature();
}

void CRayTracingPipelineGenerator::AddLibrary (IDxcBlob* DxcilLibrary, const TArray<std::wstring>& SymbolExports)
{
	Libraries.Emplace(DxcilLibrary, SymbolExports);
}

// In DXR the hit-related shaders are grouped into hit groups. Such shaders are:
// - The intersection shader, which can be used to intersect custom geometry, and is called upon
//   hitting the bounding box the the object. A default one exists to intersect triangles
// - The any hit shader, called on each intersection, which can be used to perform early
//   alpha-testing and allow the ray to continue if needed. Default is a pass-through.
// - The closest hit shader, invoked on the hit point closest to the ray start.
// The shaders in a hit group share the same root signature, and are only referred to by the
// hit group name in other places of the program.
void CRayTracingPipelineGenerator::AddHitGroup (
	const std::wstring& Name,
	const std::wstring& ClosestHitSymbol,
	const std::wstring& AnyHitSymbol,
	const std::wstring& IntersectionSymbol)
{
	HitGroups.Emplace(Name, ClosestHitSymbol, AnyHitSymbol, IntersectionSymbol);
}

void CRayTracingPipelineGenerator::AddRootSignatureAssociation (
	ID3D12RootSignature* RootSignature,
	const TArray<std::wstring>& Symbols)
{
	RootSignatureAssociations.Emplace(RootSignature, Symbols);
}

void CRayTracingPipelineGenerator::SetMaxPayloadSize (uint32 SizeInBytes)
{
	MaxPayloadSizeInBytes = SizeInBytes;
}

void CRayTracingPipelineGenerator::SetMaxAttributeSize (uint32 SizeInBytes)
{
	MaxAttributeSizeInBytes = SizeInBytes;
}

void CRayTracingPipelineGenerator::SetMaxRecursionDepth (uint32 MaxDepth)
{
	MaxRecursionDepth = MaxDepth;
}

ID3D12StateObject* CRayTracingPipelineGenerator::Generate ()
{
	// The pipeline is made of a set of sub-objects, representing the DXIL libraries, hit group
	// declarations, root signature associations, plus some configuration objects
	UINT64 subobjectCount =
		Libraries.size() +                     // DXIL libraries
		HitGroups.size() +                     // Hit group declarations
		1 +                                    // Shader configuration
		1 +                                    // Shader payload
		2 * RootSignatureAssociations.size() + // Root signature declaration + association
		2 +                                    // Empty global and local root signatures
		1;                                     // Final pipeline subobject

	// Initialize a vector with the target object count. It is necessary to make the allocation before
	// adding subobjects as some subobjects reference other subobjects by pointer. Using push_back may
	// reallocate the array and invalidate those pointers.
	std::vector<D3D12_STATE_SUBOBJECT> subobjects(subobjectCount);

	UINT currentIndex = 0;

	// Add all the DXIL libraries
	for (const SLibrary& lib : Libraries)
	{
		D3D12_STATE_SUBOBJECT libSubobject = {};
		libSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
		libSubobject.pDesc = &lib.LibDesc;

		subobjects[currentIndex++] = libSubobject;
	}

	// Add all the hit group declarations
	for (const SHitGroup& group : HitGroups)
	{
		D3D12_STATE_SUBOBJECT hitGroup = {};
		hitGroup.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
		hitGroup.pDesc = &group.Desc;

		subobjects[currentIndex++] = hitGroup;
	}

	// Add a subobject for the shader payload configuration
	D3D12_RAYTRACING_SHADER_CONFIG shaderDesc = {};
	shaderDesc.MaxPayloadSizeInBytes = MaxPayloadSizeInBytes;
	shaderDesc.MaxAttributeSizeInBytes = MaxAttributeSizeInBytes;

	D3D12_STATE_SUBOBJECT shaderConfigObject = {};
	shaderConfigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
	shaderConfigObject.pDesc = &shaderDesc;

	subobjects[currentIndex++] = shaderConfigObject;

	// Build a list of all the symbols for ray generation, miss and hit groups
	// Those shaders have to be associated with the payload definition
	TArray<std::wstring> exportedSymbols = {};
	TArray<LPCWSTR> exportedSymbolPointers = {};
	BuildShaderExportList(exportedSymbols);

	// Build an array of the string pointers
	exportedSymbolPointers.SetCapacity(exportedSymbols.size());
	for (const auto& name : exportedSymbols)
	{
		exportedSymbolPointers.Add(name.c_str());
	}
	const WCHAR** shaderExports = exportedSymbolPointers.GetData();

	// Add a subobject for the association between shaders and the payload
	D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION shaderPayloadAssociation = {};
	shaderPayloadAssociation.NumExports = static_cast<UINT>(exportedSymbols.size());
	shaderPayloadAssociation.pExports = shaderExports;

	// Associate the set of shaders with the payload defined in the previous subobject
	shaderPayloadAssociation.pSubobjectToAssociate = &subobjects[(currentIndex - 1)];

	// Create and store the payload association object
	D3D12_STATE_SUBOBJECT shaderPayloadAssociationObject = {};
	shaderPayloadAssociationObject.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
	shaderPayloadAssociationObject.pDesc = &shaderPayloadAssociation;
	subobjects[currentIndex++] = shaderPayloadAssociationObject;

	// The root signature association requires two objects for each: one to declare the root
	// signature, and another to associate that root signature to a set of symbols
	for (SRootSignatureAssociation& assoc : RootSignatureAssociations)
	{
		// Add a subobject to declare the root signature
		D3D12_STATE_SUBOBJECT rootSigObject = {};
		rootSigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
		rootSigObject.pDesc = &assoc.RootSignature;

		subobjects[currentIndex++] = rootSigObject;

		// Add a subobject for the association between the exported shader symbols and the root
		// signature
		assoc.Association.NumExports = assoc.SymbolPointers.size();
		assoc.Association.pExports = assoc.SymbolPointers.GetData();
		assoc.Association.pSubobjectToAssociate = &subobjects[(currentIndex - 1)];

		D3D12_STATE_SUBOBJECT rootSigAssociationObject = {};
		rootSigAssociationObject.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
		rootSigAssociationObject.pDesc = &assoc.Association;

		subobjects[currentIndex++] = rootSigAssociationObject;
	}

	// The pipeline construction always requires an empty global root signature
	D3D12_STATE_SUBOBJECT globalRootSig;
	globalRootSig.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
	ID3D12RootSignature* dgSig = DummyGlobalRootSignature;
	globalRootSig.pDesc = reinterpret_cast<const void*>(&dgSig);

	subobjects[currentIndex++] = globalRootSig;

	// The pipeline construction always requires an empty local root signature
	D3D12_STATE_SUBOBJECT dummyLocalRootSig;
	dummyLocalRootSig.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
	ID3D12RootSignature* dlSig = DummyLocalRootSignature;
	dummyLocalRootSig.pDesc = reinterpret_cast<const void*>(&dlSig);
	subobjects[currentIndex++] = dummyLocalRootSig;

	// Add a subobject for the ray tracing pipeline configuration
	D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
	pipelineConfig.MaxTraceRecursionDepth = MaxRecursionDepth;

	D3D12_STATE_SUBOBJECT pipelineConfigObject = {};
	pipelineConfigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
	pipelineConfigObject.pDesc = &pipelineConfig;

	subobjects[currentIndex++] = pipelineConfigObject;

	// Describe the ray tracing pipeline state object
	D3D12_STATE_OBJECT_DESC pipelineDesc = {};
	pipelineDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
	pipelineDesc.NumSubobjects = currentIndex; // static_cast<UINT>(subobjects.size());
	pipelineDesc.pSubobjects = subobjects.data();

	ID3D12StateObject* rtStateObject = nullptr;

	// Create the state object
	THROW_IF_FAILED(Device->CreateStateObject(&pipelineDesc, IID_PPV_ARGS(&rtStateObject)));
	return rtStateObject;
}

void CRayTracingPipelineGenerator::CreateDummyRootSignature ()
{
	// Creation of the global root signature
	D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
	rootDesc.NumParameters = 0;
	rootDesc.pParameters = nullptr;
	// A global root signature is the default, hence this flag
	rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	HRESULT hr = 0;

	ID3DBlob* serializedRootSignature;
	ID3DBlob* error;

	// Create the empty global root signature
	hr = D3D12SerializeRootSignature(
		&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		&serializedRootSignature, &error);
	THROW_IF_FAILED(hr);
	hr = Device->CreateRootSignature(
		0, serializedRootSignature->GetBufferPointer(),
		serializedRootSignature->GetBufferSize(),
		IID_PPV_ARGS(&DummyGlobalRootSignature));

	serializedRootSignature->Release();
	THROW_IF_FAILED(hr);

	// Create the local root signature, reusing the same descriptor but altering the creation flag
	rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
	hr = D3D12SerializeRootSignature(
		&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		&serializedRootSignature, &error);
	THROW_IF_FAILED(hr);
	hr = Device->CreateRootSignature(
		0, serializedRootSignature->GetBufferPointer(),
		serializedRootSignature->GetBufferSize(),
		IID_PPV_ARGS(&DummyLocalRootSignature));

	serializedRootSignature->Release();
	THROW_IF_FAILED(hr);
}

void CRayTracingPipelineGenerator::BuildShaderExportList (TArray<std::wstring>& OutExportedSymbols)
{
	// Get all names from libraries
	// Get names associated to hit groups
	// Return list of libraries+hit group names - shaders in hit groups

	std::unordered_set<std::wstring> exports;

	// Add all the symbols exported by the libraries
	for (const SLibrary& lib : Libraries)
	{
		for (const auto& exportName : lib.ExportedSymbols)
		{
#ifdef _DEBUG
			// Sanity check in debug mode: check that no name is exported more than once
			if (exports.find(exportName) != exports.end())
			{
				throw std::logic_error("Multiple definition of a symbol in the imported DXIL libraries");
			}
#endif
			exports.insert(exportName);
		}
	}

#ifdef _DEBUG
	// Sanity check in debug mode: verify that the hit groups do not reference an unknown shader name
	std::unordered_set<std::wstring> all_exports = exports;

	for (const auto& hitGroup : HitGroups)
	{
		if (!hitGroup.AnyHitSymbol.empty() && exports.find(hitGroup.AnyHitSymbol) == exports.end())
		{
			throw std::logic_error("Any hit symbol not found in the imported DXIL libraries");
		}

		if (!hitGroup.ClosestHitSymbol.empty() &&
			exports.find(hitGroup.ClosestHitSymbol) == exports.end())
		{
			throw std::logic_error("Closest hit symbol not found in the imported DXIL libraries");
		}

		if (!hitGroup.IntersectionSymbol.empty() &&
			exports.find(hitGroup.IntersectionSymbol) == exports.end())
		{
			throw std::logic_error("Intersection symbol not found in the imported DXIL libraries");
		}

		all_exports.insert(hitGroup.Name);
	}

	// Sanity check in debug mode: verify that the root signature associations do not reference an
	// unknown shader or hit group name
	for (const auto& assoc : RootSignatureAssociations)
	{
		for (const auto& symb : assoc.Symbols)
		{
			if (!symb.empty() && all_exports.find(symb) == all_exports.end())
			{
				throw std::logic_error(
					"Root association symbol not found in the "
					"imported DXIL libraries and hit group names");
			}
		}
	}
#endif

	// Go through all hit groups and remove the symbols corresponding to intersection, any hit and
	// closest hit shaders from the symbol set
	for (const auto& hitGroup : HitGroups)
	{
		if (!hitGroup.AnyHitSymbol.empty())
		{
			exports.erase(hitGroup.AnyHitSymbol);
		}
		if (!hitGroup.ClosestHitSymbol.empty())
		{
			exports.erase(hitGroup.ClosestHitSymbol);
		}
		if (!hitGroup.IntersectionSymbol.empty())
		{
			exports.erase(hitGroup.IntersectionSymbol);
		}
		exports.insert(hitGroup.Name);
	}

	// Finally build a vector containing ray generation and miss shaders, plus the hit group names
	for (const auto& name : exports)
	{
		OutExportedSymbols.Add(name);
	}
}

CRayTracingPipelineGenerator::SLibrary::SLibrary (IDxcBlob* InDxil, const TArray<std::wstring>& InExportedSymbols)
	: Dxil(InDxil)
	, ExportedSymbols(InExportedSymbols)
{
	Exports.SetSizeUninitialized(ExportedSymbols.Count());

	for (uint32 i = 0; i < ExportedSymbols.Count(); ++i)
	{
		Exports[i] = {};
		Exports[i].Name = ExportedSymbols[i].c_str();
		Exports[i].ExportToRename = nullptr;
		Exports[i].Flags = D3D12_EXPORT_FLAG_NONE;
	}

	LibDesc.DXILLibrary.BytecodeLength = Dxil->GetBufferSize();
	LibDesc.DXILLibrary.pShaderBytecode = Dxil->GetBufferPointer();
	LibDesc.NumExports = ExportedSymbols.Count();
	LibDesc.pExports = Exports.GetData();
}

CRayTracingPipelineGenerator::SLibrary::SLibrary (const SLibrary& Source)
	: SLibrary(Source.Dxil, Source.ExportedSymbols) {}

CRayTracingPipelineGenerator::SHitGroup::SHitGroup (
	std::wstring InName,
	std::wstring ClosestHit,
	std::wstring AnyHit,
	std::wstring Intersection)
	: Name(std::move(InName))
	, ClosestHitSymbol(std::move(ClosestHit))
	, AnyHitSymbol(std::move(AnyHit))
	, IntersectionSymbol(std::move(Intersection))
{
	Desc.HitGroupExport = Name.c_str();
	Desc.ClosestHitShaderImport = ClosestHitSymbol.empty() ? nullptr : ClosestHitSymbol.c_str();
	Desc.AnyHitShaderImport = AnyHitSymbol.empty() ? nullptr : AnyHitSymbol.c_str();
	Desc.IntersectionShaderImport = IntersectionSymbol.empty() ? nullptr : IntersectionSymbol.c_str();
}

CRayTracingPipelineGenerator::SHitGroup::SHitGroup (const SHitGroup& Source)
	: SHitGroup(Source.Name, Source.ClosestHitSymbol, Source.AnyHitSymbol, Source.IntersectionSymbol)
{}

CRayTracingPipelineGenerator::SRootSignatureAssociation::SRootSignatureAssociation (
	ID3D12RootSignature* InRootSignature,
	const TArray<std::wstring>& InSymbols)
	: RootSignature(InRootSignature)
	, Symbols(InSymbols)

{
	SymbolPointers.SetSizeUninitialized(Symbols.Count());

	for (uint32 i = 0u; i < Symbols.Count(); ++i)
	{
		SymbolPointers[i] = Symbols[i].c_str();
	}
	RootSignaturePointer = RootSignature;
}

CRayTracingPipelineGenerator::SRootSignatureAssociation::SRootSignatureAssociation (
	const SRootSignatureAssociation& Source)
	: SRootSignatureAssociation(Source.RootSignature, Source.Symbols)
{}

void CShaderBindingTableGenerator::AddRayGenerationProgram (
	const std::wstring& EntryPoint,
	const TArray<void*>& InputData)
{
	RayGen.Emplace(EntryPoint, InputData);
}

void CShaderBindingTableGenerator::AddMissProgram (const std::wstring& EntryPoint, const TArray<void*>& InputData)
{
	Miss.Emplace(EntryPoint, InputData);
}

void CShaderBindingTableGenerator::AddHitGroup (const std::wstring& EntryPoint, const TArray<void*>& InputData)
{
	HitGroup.Emplace(EntryPoint, InputData);
}

uint32 CShaderBindingTableGenerator::ComputeSBTSize ()
{
	ProgIdSize = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;

	RayGenEntrySize = GetEntrySize(RayGen);
	MissEntrySize = GetEntrySize(Miss);
	HitGroupEntrySize = GetEntrySize(HitGroup);

	const uint32 sbtSize = memory::AlignAddress(
		RayGenEntrySize * RayGen.Count() +
		MissEntrySize * Miss.Count() +
		HitGroupEntrySize * HitGroup.Count(),
		256u);

	return sbtSize;
}

ID3D12Resource* CShaderBindingTableGenerator::Generate (
	ID3D12Resource* SbtBuffer,
	ID3D12StateObjectProperties* RaytracingPipeline)
{
	// Map the SBT
	uint8* pData;
	HRESULT hr = SbtBuffer->Map(0, nullptr, reinterpret_cast<void**>(&pData));
	if (FAILED(hr))
	{
		throw std::logic_error("Could not map the shader binding table");
	}
	// Copy the shader identifiers followed by their resource pointers or root constants: first the
	// ray generation, then the miss shaders, and finally the set of hit groups
	uint32 offset = 0;

	offset = CopyShaderData(RaytracingPipeline, pData, RayGen, RayGenEntrySize);
	pData += offset;

	offset = CopyShaderData(RaytracingPipeline, pData, Miss, MissEntrySize);
	pData += offset;

	offset = CopyShaderData(RaytracingPipeline, pData, HitGroup, HitGroupEntrySize);

	// Unmap the SBT
	SbtBuffer->Unmap(0, nullptr);
}

void CShaderBindingTableGenerator::Reset ()
{
	RayGen.Clear();
	Miss.Clear();
	HitGroup.Clear();

	RayGenEntrySize = 0u;
	MissEntrySize = 0u;
	HitGroupEntrySize = 0u;
	ProgIdSize = 0u;
}

uint32 CShaderBindingTableGenerator::GetRayGenSectionSize ()
{
	return RayGenEntrySize * RayGen.Count();
}

uint32 CShaderBindingTableGenerator::GetRayGenEntrySize ()
{
	return RayGenEntrySize;
}

uint32 CShaderBindingTableGenerator::GetMissSectionSize ()
{
	return MissEntrySize * Miss.Count();
}

uint32 CShaderBindingTableGenerator::GetMissEntrySize ()
{
	return MissEntrySize;
}

uint32 CShaderBindingTableGenerator::GetHitGroupSectionSize ()
{
	return HitGroupEntrySize * HitGroup.Count();
}

uint32 CShaderBindingTableGenerator::GetHitGroupEntrySize ()
{
	return HitGroupEntrySize;
}


uint32 CShaderBindingTableGenerator::CopyShaderData (
	ID3D12StateObjectProperties* RaytracingPipeline,
	uint8* OutputData,
	const TArray<SSBTEntry>& Shaders,
	uint32 EntrySize)
{
	uint8_t* pData = OutputData;
	for (const auto& shader : Shaders)
	{
		// Get the shader identifier, and check whether that identifier is known
		void* id = RaytracingPipeline->GetShaderIdentifier(shader.EntryPoint.c_str());
		if (!id)
		{
			std::wstring errMsg(
				std::wstring(L"Unknown shader identifier used in the SBT: ") +
				shader.EntryPoint);
			throw std::logic_error(std::string(errMsg.begin(), errMsg.end()));
		}
		// Copy the shader identifier
		memcpy(pData, id, ProgIdSize);
		// Copy all its resources pointers or values in bulk
		memcpy(pData + ProgIdSize, shader.InputData.GetData(), shader.InputData.Count() * 8);

		pData += EntrySize;
	}
	// Return the number of bytes actually written to the output buffer
	return Shaders.Count() * EntrySize;
}

uint32 CShaderBindingTableGenerator::GetEntrySize (const TArray<SSBTEntry>& Entries)
{
	// Find the maximum number of parameters used by a single entry
	uint32 maxArgs = 0;
	for (const auto& shader : Entries)
	{
		maxArgs = std::max(maxArgs, shader.InputData.Count());
	}
	// A SBT entry is made of a program ID and a set of parameters, taking 8 bytes each. Those
	// parameters can either be 8-bytes pointers, or 4-bytes constants
	uint32_t entrySize = ProgIdSize + 8 * maxArgs;

	// The entries of the shader binding table must be 16-bytes-aligned
	entrySize = memory::AlignAddress(entrySize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

	return entrySize;
}

CShaderBindingTableGenerator::SSBTEntry::SSBTEntry (const std::wstring& InEntryPoint, const TArray<void*>& InInputData)
	: EntryPoint(InEntryPoint)
	, InputData(std::move(InInputData))
{}

void CRootSignatureGenerator::AddHeapRangesParameter (const TArray<D3D12_DESCRIPTOR_RANGE>& InRanges)
{
	Ranges.Add(InRanges);

	// A set of ranges on the heap is a descriptor table parameter
	D3D12_ROOT_PARAMETER param = {};
	param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	param.DescriptorTable.NumDescriptorRanges = InRanges.Count();
	// The range pointer is kept null here, and will be resolved when generating the root signature
	// (see explanation of RangeLocations below)
	param.DescriptorTable.pDescriptorRanges = nullptr;

	// All parameters (heap ranges and root parameters) are added to the same parameter list to
	// preserve order
	Parameters.Add(param);

	// The descriptor table descriptor ranges require a pointer to the descriptor ranges. Since new
	// ranges can be dynamically added in the vector, we separately store the index of the range set.
	// The actual address will be solved when generating the actual root signature
	RangeLocations.Add(Ranges.Count() - 1);
}

void CRootSignatureGenerator::AddHeapRangesParameter (
	std::vector<std::tuple<uint32, uint32, uint32, D3D12_DESCRIPTOR_RANGE_TYPE, uint32>> InRanges)
{
	// Build and store the set of descriptors for the ranges
	std::vector<D3D12_DESCRIPTOR_RANGE> rangeStorage;
	for (const auto& input : InRanges)
	{
		D3D12_DESCRIPTOR_RANGE r = {};
		r.BaseShaderRegister = std::get<RSC_BASE_SHADER_REGISTER>(input);
		r.NumDescriptors = std::get<RSC_NUM_DESCRIPTORS>(input);
		r.RegisterSpace = std::get<RSC_REGISTER_SPACE>(input);
		r.RangeType = std::get<RSC_RANGE_TYPE>(input);
		r.OffsetInDescriptorsFromTableStart =
			std::get<RSC_OFFSET_IN_DESCRIPTORS_FROM_TABLE_START>(input);
		rangeStorage.push_back(r);
	}

	// Add those ranges to the heap parameters
	AddHeapRangesParameter(rangeStorage);
}

void CRootSignatureGenerator::AddRootParameter (
	D3D12_ROOT_PARAMETER_TYPE Type,
	uint32 ShaderRegister,
	uint32 RegisterSpace,
	uint32 NumRootConstants)
{
	D3D12_ROOT_PARAMETER param = {};
	param.ParameterType = Type;
	// The descriptor is an union, so specific values need to be set in case the parameter is a
	// constant instead of a buffer.
	if (Type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
	{
		param.Constants.Num32BitValues = NumRootConstants;
		param.Constants.RegisterSpace = RegisterSpace;
		param.Constants.ShaderRegister = ShaderRegister;
	}
	else
	{
		param.Descriptor.RegisterSpace = RegisterSpace;
		param.Descriptor.ShaderRegister = ShaderRegister;
	}

	// We default the visibility to all shaders
	param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	// Add the root parameter to the set of parameters,
	Parameters.Add(param);
	// and indicate that there will be no range
	// location to indicate since this parameter is not part of the heap
	RangeLocations.Add(~0u);
}

void CRootSignatureGenerator::AddStaticSampler (const D3D12_STATIC_SAMPLER_DESC& InStaticSampler)
{
	StaticSamplers.Add(InStaticSampler);
}

ID3D12RootSignature* CRootSignatureGenerator::Generate (ID3D12Device* Device, bool bLocal)
{
	// Go through all the parameters, and set the actual addresses of the heap range descriptors based
	// on their indices in the range set array
	for (size_t i = 0; i < Parameters.size(); i++)
	{
		if (Parameters[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
		{
			Parameters[i].DescriptorTable.pDescriptorRanges = Ranges[RangeLocations[i]].GetData();
		}
	}
	// Specify the root signature with its set of parameters
	D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
	rootDesc.NumParameters = Parameters.size();
	rootDesc.pParameters = Parameters.GetData();
	rootDesc.NumStaticSamplers = StaticSamplers.Count();
	rootDesc.pStaticSamplers = StaticSamplers.GetData();
	// Set the flags of the signature. By default root signatures are global, for example for vertex
	// and pixel shaders. For raytracing shaders the root signatures are local.
	rootDesc.Flags =
		bLocal ? D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE : D3D12_ROOT_SIGNATURE_FLAG_NONE;

	// Create the root signature from its descriptor
	ID3DBlob* pSigBlob;
	ID3DBlob* pErrorBlob;
	HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &pSigBlob,
											 &pErrorBlob);
	if (FAILED(hr))
	{
		throw std::logic_error("Cannot serialize root signature");
	}
	ID3D12RootSignature* pRootSig;
	hr = Device->CreateRootSignature(0, pSigBlob->GetBufferPointer(), pSigBlob->GetBufferSize(),
									 IID_PPV_ARGS(&pRootSig));
	if (FAILED(hr))
	{
		throw std::logic_error("Cannot create root signature");
	}
	return pRootSig;
}

ID3D12Resource* CreateBuffer (
	ID3D12Device* Device,
	uint64 SizeInBytes,
	D3D12_RESOURCE_FLAGS Flags,
	D3D12_RESOURCE_STATES InitialState,
	const D3D12_HEAP_PROPERTIES& HeapProps)
{
	if ((Flags & D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE) ||
		InitialState == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
	{
		Flags |= D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
		Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}
	const D3D12_RESOURCE_DESC bufDesc
	{
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Alignment = 0,
		.Width = SizeInBytes,
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_UNKNOWN,
		.SampleDesc = DXGI_SAMPLE_DESC
		{
			.Count = 1,
			.Quality = 0
		},
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
		.Flags = Flags
	};

	ID3D12Resource* buffer;
	D3D12_RESOURCE_STATES createState = InitialState;
	if ((Flags & D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE) ||
		InitialState == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
	{
		createState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
	}
	THROW_IF_FAILED(
		Device->CreateCommittedResource(
			&HeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, createState, nullptr, IID_PPV_ARGS(&buffer)));
	return buffer;
}
}
