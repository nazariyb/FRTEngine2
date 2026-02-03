/*-----------------------------------------------------------------------
Copyright (c) 2014-2018, NVIDIA. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
* Neither the name of its contributors may be used to endorse
or promote products derived from this software without specific
prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
-----------------------------------------------------------------------*/

#pragma once

#include <DirectXMath.h>
#include <string>

#include "Containers/Array.h"
#include "Graphics/Render/RenderResourceAllocators.h"


struct IDxcBlob;


namespace frt::graphics::raytracing
{
/*
	The bottom-level hierarchy is used to store the triangle data in a way suitable
	for fast ray-triangle intersection at runtime. To be built, this data structure
	requires some scratch space which has to be allocated by the application.
	Similarly, the resulting data structure is stored in an application-controlled
	buffer.

	To be used, the application must first add all the vertex buffers to be
	contained in the final structure, using AddVertexBuffer. After all buffers have
	been added, ComputeASBufferSizes will prepare the build, and provide the
	required sizes for the scratch data and the final result. The Build call will
	finally compute the acceleration structure and store it in the result buffer.

	Note that the build is enqueued in the command list, meaning that the scratch
	buffer needs to be kept until the command list execution is finished.


	Example:

	BottomLevelASGenerator bottomLevelAS;
	bottomLevelAS.AddVertexBuffer(vertexBuffer, 0, vertexCount, sizeof(Vertex),
	identityMat.Get(), 0); bottomLevelAS.AddVertexBuffer(vertexBuffer2, 0,
	vertexCount2, sizeof(Vertex), identityMat.Get(), 0);
	...
	UINT64 scratchSizeInBytes = 0;
	UINT64 resultSizeInBytes = 0;
	bottomLevelAS.ComputeASBufferSizes(GetRTDevice(), false, &scratchSizeInBytes,
	&resultSizeInBytes); AccelerationStructureBuffers buffers; buffers.pScratch =
	nv_helpers_dx12::CreateBuffer(..., scratchSizeInBytes, ...); buffers.pResult =
	nv_helpers_dx12::CreateBuffer(..., resultSizeInBytes, ...);

	bottomLevelAS.Generate(m_commandList.Get(), rtCmdList, buffers.pScratch.Get(),
	buffers.pResult.Get(), false, nullptr);

	return buffers;
 */
class CBottomLevelASGenerator
{
public:
	void AddVertexBuffer (
		ID3D12Resource* VertexBuffer,
		uint64 VertexBufferOffset,
		uint32 VertexCount,
		uint32 VertexSize,
		ID3D12Resource* IndexBuffer,
		uint64 IndexBufferOffset,
		uint32 IndexCount,
		ID3D12Resource* TransformBuffer,
		uint64 TransformBufferOffset,
		bool bOpaque = true);

	void ComputeASBufferSizes (
		ID3D12Device5* Device,
		bool bAllowUpdate,
		uint64* OutScratchSizeInBytes,
		uint64* OutResultSizeInBytes);

	void Generate (
		ID3D12GraphicsCommandList4* CommandList,
		ID3D12Resource* ScratchBuffer,
		ID3D12Resource* ResultBuffer,
		bool bUpdateOnly = false,
		ID3D12Resource* PreviousResult = nullptr);

private:
	TArray<D3D12_RAYTRACING_GEOMETRY_DESC> VertexBuffers;
	uint64 ScratchSizeInBytes = 0;
	uint64 ResultSizeInBytes = 0;
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
};


/*
	The top-level hierarchy is used to store a set of instances represented by
	bottom-level hierarchies in a way suitable for fast intersection at runtime. To
	be built, this data structure requires some scratch space which has to be
	allocated by the application. Similarly, the resulting data structure is stored
	in an application-controlled buffer.

	To be used, the application must first add all the instances to be contained in
	the final structure, using AddInstance. After all instances have been added,
	ComputeASBufferSizes will prepare the build, and provide the required sizes for
	the scratch data and the final result. The Build call will finally compute the
	acceleration structure and store it in the result buffer.

	Note that the build is enqueued in the command list, meaning that the scratch
	buffer needs to be kept until the command list execution is finished.



	Example:

	TopLevelASGenerator topLevelAS;
	topLevelAS.AddInstance(instances1, matrix1, instanceId1, hitGroupIndex1);
	topLevelAS.AddInstance(instances2, matrix2, instanceId2, hitGroupIndex2);
	...
	UINT64 scratchSize, resultSize, instanceDescsSize;
	topLevelAS.ComputeASBufferSizes(GetRTDevice(), true, &scratchSize, &resultSize,
	&instanceDescsSize); AccelerationStructureBuffers buffers; buffers.pScratch =
	nv_helpers_dx12::CreateBuffer(..., scratchSizeInBytes, ...); buffers.pResult =
	nv_helpers_dx12::CreateBuffer(..., resultSizeInBytes, ...);
	buffers.pInstanceDesc = nv_helpers_dx12::CreateBuffer(..., resultSizeInBytes,
	...); topLevelAS.Generate(m_commandList.Get(), rtCmdList,
	m_topLevelAS.pScratch.Get(), m_topLevelAS.pResult.Get(),
	m_topLevelAS.pInstanceDesc.Get(), updateOnly, updateOnly ?
	m_topLevelAS.pResult.Get() : nullptr);

	return buffers;
 */
class CTopLevelASGenerator
{
public:
	void AddInstance (
		ID3D12Resource* BottomLevelAS,
		const DirectX::XMMATRIX& Transform,
		uint32 InstanceID,
		uint32 HitGroupIndex);

	void ComputeASBufferSizes (
		ID3D12Device5* Device,
		bool bAllowUpdate,
		uint64* OutScratchSizeInBytes,
		uint64* OutResultSizeInBytes,
		uint64* OutDescriptorSizeInBytes);

	void Generate (
		ID3D12GraphicsCommandList4* CommandList,
		ID3D12Resource* ScratchBuffer,
		ID3D12Resource* ResultBuffer,
		ID3D12Resource* DescriptorsBuffer,
		bool bUpdateOnly = false,
		ID3D12Resource* PreviousResult = nullptr);

private:
	struct SInstance
	{
		ID3D12Resource* BottomLevelAS = nullptr;
		DirectX::XMMATRIX Transform = {};
		uint32 InstanceID = 0;
		uint32 HitGroupIndex = 0;
	};


	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
	TArray<SInstance> Instances;

	uint64 ScratchSizeInBytes = 0;
	uint64 ResultSizeInBytes = 0;
	uint64 DescriptorSizeInBytes = 0;
};


/*
	The raytracing pipeline combines the raytracing shaders into a state object,
	that can be thought of as an executable GPU program. For that, it requires the
	shaders compiled as DXIL libraries, where each library exports symbols in a way
	similar to DLLs. Those symbols are then used to refer to these shaders libraries
	when creating hit groups, associating the shaders to their root signatures and
	declaring the steps of the pipeline. All the calls to this helper class can be
	done in arbitrary order. Some basic sanity checks are also performed when
	compiling in debug mode.

	Simple usage of this class:

	pipeline.AddLibrary(m_rayGenLibrary.Get(), {L"RayGen"});
	pipeline.AddLibrary(m_missLibrary.Get(), {L"Miss"});
	pipeline.AddLibrary(m_hitLibrary.Get(), {L"ClosestHit"});

	pipeline.AddHitGroup(L"HitGroup", L"ClosestHit");

	pipeline.AddRootSignatureAssociation(m_rayGenSignature.Get(), {L"RayGen"});
	pipeline.AddRootSignatureAssociation(m_missSignature.Get(), {L"Miss"});
	pipeline.AddRootSignatureAssociation(m_hitSignature.Get(), {L"HitGroup"});

	pipeline.SetMaxPayloadSize(4 * sizeof(float)); // RGB + distance

	pipeline.SetMaxAttributeSize(2 * sizeof(float)); // barycentric coordinates

	pipeline.SetMaxRecursionDepth(1);

	rtStateObject = pipeline.Generate();
 */
class CRayTracingPipelineGenerator
{
public:
	CRayTracingPipelineGenerator (ID3D12Device5* InDevice);

	void AddLibrary (IDxcBlob* DxcilLibrary, const TArray<std::wstring>& SymbolExports);
	void AddHitGroup (
		const std::wstring& Name,
		const std::wstring& ClosestHitSymbol,
		const std::wstring& AnyHitSymbol = L"",
		const std::wstring& IntersectionSymbol = L"");
	void AddRootSignatureAssociation (ID3D12RootSignature* RootSignature, const TArray<std::wstring>& Symbols);

	void SetMaxPayloadSize (uint32 SizeInBytes);
	void SetMaxAttributeSize (uint32 SizeInBytes);
	void SetMaxRecursionDepth (uint32 MaxDepth);

	ID3D12StateObject* Generate ();

private:
	struct SLibrary
	{
		SLibrary (IDxcBlob* InDxil, const TArray<std::wstring>& InExportedSymbols);
		SLibrary (const SLibrary& Source);

		IDxcBlob* Dxil = nullptr;
		const TArray<std::wstring> ExportedSymbols;

		TArray<D3D12_EXPORT_DESC> Exports;
		D3D12_DXIL_LIBRARY_DESC LibDesc = {};
	};


	struct SHitGroup
	{
		SHitGroup (
			std::wstring InName,
			std::wstring ClosestHit,
			std::wstring AnyHit = L"",
			std::wstring Intersection = L"");
		SHitGroup (const SHitGroup& Source);

		std::wstring Name;
		std::wstring ClosestHitSymbol;
		std::wstring AnyHitSymbol;
		std::wstring IntersectionSymbol;
		D3D12_HIT_GROUP_DESC Desc = {};
	};


	struct SRootSignatureAssociation
	{
		SRootSignatureAssociation (ID3D12RootSignature* InRootSignature, const TArray<std::wstring>& InSymbols);
		SRootSignatureAssociation (const SRootSignatureAssociation& Source);

		ID3D12RootSignature* RootSignature;
		ID3D12RootSignature* RootSignaturePointer;
		TArray<std::wstring> Symbols;
		TArray<LPCWSTR> SymbolPointers;
		D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION Association = {};
	};


	void CreateDummyRootSignature ();
	void BuildShaderExportList (TArray<std::wstring>& OutExportedSymbols);

	TArray<SLibrary> Libraries;
	TArray<SHitGroup> HitGroups;
	TArray<SRootSignatureAssociation> RootSignatureAssociations;

	uint32 MaxPayloadSizeInBytes = 0;
	/**
	 * Attribute size, initialized to 2 for the barycentric coordinates used by the built-in triangle
	 * intersection shader
	 */
	uint32 MaxAttributeSizeInBytes = 2 * sizeof(float);
	uint32 MaxRecursionDepth = 0;

	ID3D12Device5* Device = nullptr;
	ID3D12RootSignature* DummyLocalRootSignature = nullptr;
	ID3D12RootSignature* DummyGlobalRootSignature = nullptr;
};


/*
	The ShaderBindingTable is a helper to construct the SBT. It helps to maintain the
	proper offsets of each element, required when constructing the SBT, but also when filling the
	dispatch rays description.

	Example:


	D3D12_GPU_DESCRIPTOR_HANDLE srvUavHeapHandle = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();
	UINT64* heapPointer = reinterpret_cast< UINT64* >(srvUavHeapHandle.ptr);

	m_sbtHelper.AddRayGenerationProgram(L"RayGen", {heapPointer});
	m_sbtHelper.AddMissProgram(L"Miss", {});

	m_sbtHelper.AddHitGroup(L"HitGroup",
	{(void*)(m_constantBuffers[i]->GetGPUVirtualAddress())});
	m_sbtHelper.AddHitGroup(L"ShadowHitGroup", {});


	// Create the SBT on the upload heap
	uint32_t sbtSize = 0;
	m_sbtHelper.ComputeSBTSize(GetRTDevice(), &sbtSize);
	m_sbtStorage = nv_helpers_dx12::CreateBuffer(m_device.Get(), sbtSize,
	D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
	nv_helpers_dx12::kUploadHeapProps);

	m_sbtHelper.Generate(m_sbtStorage.Get(), m_rtStateObjectProps.Get());


	//--------------------------------------------------------------------
	Then setting the descriptor for the dispatch rays become way easier
	//--------------------------------------------------------------------

	D3D12_DISPATCH_RAYS_DESC desc = {};
	// The layout of the SBT is as follows: ray generation shaders, miss shaders,
	hit groups. As
	// described in the CreateShaderBindingTable method, all SBT entries have the
	same size to allow
	// a fixed stride.

	// The ray generation shaders are always at the beginning of the SBT. In this
	// example we have only one RG, so the size of this SBT sections is
	m_sbtEntrySize. uint32_t rayGenerationSectionSizeInBytes =
	m_sbtHelper.GetRayGenSectionSize(); desc.RayGenerationShaderRecord.StartAddress
	= m_sbtStorage->GetGPUVirtualAddress();
	desc.RayGenerationShaderRecord.SizeInBytes = rayGenerationSectionSizeInBytes;

	// The miss shaders are in the second SBT section, right after the ray
	generation shader. We
	// have one miss shader for the camera rays and one for the shadow rays, so this
	section has a
	// size of 2*m_sbtEntrySize. We also indicate the stride between the two miss
	shaders, which is
	// the size of a SBT entry
	uint32_t missSectionSizeInBytes = m_sbtHelper.GetMissSectionSize();
	desc.MissShaderTable.StartAddress =
	m_sbtStorage->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes;
	desc.MissShaderTable.SizeInBytes = missSectionSizeInBytes;
	desc.MissShaderTable.StrideInBytes = m_sbtHelper.GetMissEntrySize();

	// The hit groups section start after the miss shaders. In this sample we have 4
	hit groups: 2
	// for the triangles (1 used when hitting the geometry from a camera ray, 1 when
	hitting the
	// same geometry from a shadow ray) and 2 for the plane. We also indicate the
	stride between the
	// two miss shaders, which is the size of a SBT entry
	// #Pascal: experiment with different sizes for the SBT entries
	uint32_t hitGroupsSectionSize = m_sbtHelper.GetHitGroupSectionSize();
	desc.HitGroupTable.StartAddress =
	m_sbtStorage->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes +
	missSectionSizeInBytes; desc.HitGroupTable.SizeInBytes = hitGroupsSectionSize;
	desc.HitGroupTable.StrideInBytes = m_sbtHelper.GetHitGroupEntrySize();
 */
class CShaderBindingTableGenerator
{
public:
	/**
	 * Add a ray generation program by name, with its list of data pointers or values according to the layout of its root signature
	 */
	void AddRayGenerationProgram (const std::wstring& EntryPoint, const TArray<void*>& InputData);

	/**
	 * Add a miss program by name, with its list of data pointers or values according to the layout of its root signature
	 */
	void AddMissProgram (const std::wstring& EntryPoint, const TArray<void*>& InputData);

	/**
	 * Add a hit group by name, with its list of data pointers or values according to the layout of its root signature
	 */
	void AddHitGroup (const std::wstring& EntryPoint, const TArray<void*>& InputData);

	uint32 ComputeSBTSize ();

	ID3D12Resource* Generate (ID3D12Resource* SbtBuffer, ID3D12StateObjectProperties* RaytracingPipeline);

	void Reset ();

	uint32 GetRayGenSectionSize ();
	uint32 GetRayGenEntrySize ();

	uint32 GetMissSectionSize ();
	uint32 GetMissEntrySize ();

	uint32 GetHitGroupSectionSize ();
	uint32 GetHitGroupEntrySize ();

private:
	struct SSBTEntry
	{
		SSBTEntry (const std::wstring& InEntryPoint, const TArray<void*>& InInputData);
		std::wstring EntryPoint;
		TArray<void*> InputData;
	};


	uint32 CopyShaderData (
		ID3D12StateObjectProperties* RaytracingPipeline,
		uint8* OutputData,
		const TArray<SSBTEntry>& Shaders,
		uint32 EntrySize);
	uint32 GetEntrySize (const TArray<SSBTEntry>& Entries);

	TArray<SSBTEntry> RayGen;
	TArray<SSBTEntry> Miss;
	TArray<SSBTEntry> HitGroup;

	uint32 RayGenEntrySize = 0u;
	uint32 MissEntrySize = 0u;
	uint32 HitGroupEntrySize = 0u;

	uint32 ProgIdSize = 0u;
};


/*
	Utility class to create root signatures. The order in which the addition methods are called is
	important as it will define the slots of the heap or of the Shader Binding Table to which buffer
	pointers will be bound.

	Example to create an empty root signature:
	nv_helpers_dx12::RootSignatureGenerator rsc;
	return rsc.Generate(m_device.Get(), true);

	Example to create a signature with one constant buffer:
	nv_helpers_dx12::RootSignatureGenerator rsc;
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV);
	return rsc.Generate(m_device.Get(), true);

	More advance root signature:
	nv_helpers_dx12::RootSignatureGenerator rsc;
	rsc.AddRangeParameter({{0,1,0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0},
	{0,1,0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1},
	{0,1,0, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 2}});
	return rsc.Generate(m_device.Get(), true);
 */
class CRootSignatureGenerator
{
public:
	void AddHeapRangesParameter (const TArray<D3D12_DESCRIPTOR_RANGE>& InRanges);

	/// Add a set of heap ranges as a parameter of the root signature. Each range
	/// is defined as follows:
	/// - UINT BaseShaderRegister: the first register index in the range, e.g. the
	/// register of a UAV with BaseShaderRegister==0 is defined in the HLSL code
	/// as register(u0)
	/// - UINT NumDescriptors: number of descriptors in the range. Those will be
	/// mapped to BaseShaderRegister, BaseShaderRegister+1 etc. UINT
	/// RegisterSpace: Allows using the same register numbers multiple times by
	/// specifying a space where they are defined, similarly to a namespace. For
	/// example, a UAV with BaseShaderRegister==0 and RegisterSpace==1 is accessed
	/// in HLSL using the syntax register(u0, space1)
	/// - D3D12_DESCRIPTOR_RANGE_TYPE RangeType: The range type, such as
	/// D3D12_DESCRIPTOR_RANGE_TYPE_CBV for a constant buffer
	/// - UINT OffsetInDescriptorsFromTableStart: The first slot in the heap
	/// corresponding to the buffers mapped by the root signature. This can either
	/// be explicit, or implicit using D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND. In
	/// this case the index in the heap is the one directly following the last
	/// parameter range (or 0 if it's the first)
	void AddHeapRangesParameter (
		std::vector<std::tuple<uint32,                       // BaseShaderRegister,
								uint32,                      // NumDescriptors
								uint32,                      // RegisterSpace
								D3D12_DESCRIPTOR_RANGE_TYPE, // RangeType
								uint32                       // OffsetInDescriptorsFromTableStart
		>>
		InRanges);

	/// Add a root parameter to the shader, defined by its type: constant buffer (CBV), shader
	/// resource (SRV), unordered access (UAV), or root constant (CBV, directly defined by its value
	/// instead of a buffer). The shaderRegister and registerSpace indicate how to access the
	/// parameter in the HLSL code, e.g a SRV with shaderRegister==1 and registerSpace==0 is
	/// accessible via register(t1, space0).
	/// In case of a root constant, the last parameter indicates how many successive 32-bit constants
	/// will be bound.
	void AddRootParameter (
		D3D12_ROOT_PARAMETER_TYPE Type,
		uint32 ShaderRegister = 0u,
		uint32 RegisterSpace = 0u,
		uint32 NumRootConstants = 1u);

	ID3D12RootSignature* Generate (ID3D12Device* Device, bool bLocal);

private:
	TArray<TArray<D3D12_DESCRIPTOR_RANGE>> Ranges;
	TArray<D3D12_ROOT_PARAMETER> Parameters;

	TArray<uint32> RangeLocations;


	enum
	{
		RSC_BASE_SHADER_REGISTER                   = 0,
		RSC_NUM_DESCRIPTORS                        = 1,
		RSC_REGISTER_SPACE                         = 2,
		RSC_RANGE_TYPE                             = 3,
		RSC_OFFSET_IN_DESCRIPTORS_FROM_TABLE_START = 4
	};
};


inline static const D3D12_HEAP_PROPERTIES DefaultHeapProps =
{
	D3D12_HEAP_TYPE_DEFAULT,
	D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
	D3D12_MEMORY_POOL_UNKNOWN,
	0,
	0
};
inline static const D3D12_HEAP_PROPERTIES UploadHeapProps =
{
	D3D12_HEAP_TYPE_UPLOAD,
	D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
	D3D12_MEMORY_POOL_UNKNOWN,
	0,
	0
};

ID3D12Resource* CreateBuffer (
	ID3D12Device* Device,
	uint64 SizeInBytes,
	D3D12_RESOURCE_FLAGS Flags,
	D3D12_RESOURCE_STATES InitialState,
	const D3D12_HEAP_PROPERTIES& HeapProps);
}
