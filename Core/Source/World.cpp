#include "World.h"

#include <unordered_map>

#include "Exception.h"
#include "GameInstance.h"
#include "Timer.h"
#include "Window.h"
#include "Graphics/Camera.h"
#include "Graphics/DXRUtils.h"
#include "Graphics/Render/GraphicsCoreTypes.h"
#include "Graphics/Render/Renderer.h"

using namespace frt;

#if !defined(FRT_HEADLESS)
CWorld::CWorld (memory::TRefWeak<graphics::CRenderer> InRenderer)
	: Renderer(InRenderer)
{}
#else
CWorld::CWorld ()
{}
#endif

void CWorld::Tick (float DeltaSeconds)
{
	for (auto& entity : Entities)
	{
		entity->Tick(DeltaSeconds);
	}
#if !defined(FRT_HEADLESS)
	CopyConstantData();
#endif
}

#if !defined(FRT_HEADLESS)
void CWorld::Present (float DeltaSeconds, ID3D12GraphicsCommandList4* CommandList)
{
	auto& currentFrameResources = Renderer->GetCurrentFrameResource();

	// TODO: assign stable material indices in MaterialLibrary and update constants only when dirty.
	std::unordered_map<const graphics::SMaterial*, uint32> materialIndices;
	TArray<graphics::SMaterialConstants> materialConstants;

	for (uint32 i = 0; i < Entities.Count(); ++i)
	{
		CEntity* entity = Entities[i].GetRawIgnoringLifetime();
		frt_assert(entity);
		if (!entity->RenderModel.Model)
		{
			continue;
		}

		graphics::SRenderModel& model = *entity->RenderModel.Model;
		for (const graphics::SRenderSection& section : model.Sections)
		{
			if (section.MaterialIndex >= model.Materials.Count())
			{
				continue;
			}

			graphics::SMaterial* material = model.Materials[section.MaterialIndex].GetRawIgnoringLifetime();
			frt_assert(material);

			auto it = materialIndices.find(material);
			if (it == materialIndices.end())
			{
				const uint32 materialIndex = materialConstants.Count();
				materialIndices.emplace(material, materialIndex);

				graphics::SMaterialConstants& constants = materialConstants.Add();
				constants.DiffuseAlbedo = material->DiffuseAlbedo;
				material->RuntimeIndex = materialIndex;
			}
			else
			{
				material->RuntimeIndex = it->second;
			}
		}
	}

	Renderer->EnsureMaterialConstantCapacity(materialConstants.Count());
	if (!materialConstants.IsEmpty())
	{
		currentFrameResources.MaterialCB.CopyBunch(
			materialConstants.GetData(),
			materialConstants.Count(),
			currentFrameResources.UploadArena);
	}

	// this is so messed up...
	currentFrameResources.PassCB.Upload(currentFrameResources.UploadArena, Renderer->GetCommandList());
	if (!currentFrameResources.PassCB.DescriptorHeapHandleGpu.empty())
	{
		CommandList->SetGraphicsRootDescriptorTable(
			render::constants::RootParam_PassCbv,
			currentFrameResources.PassCB.DescriptorHeapHandleGpu[0]);
	}

	currentFrameResources.ObjectCB.Upload(currentFrameResources.UploadArena, Renderer->GetCommandList());
	currentFrameResources.MaterialCB.Upload(currentFrameResources.UploadArena, Renderer->GetCommandList());

	memory::TRefWeak<graphics::CRenderer> renderer = GameInstance::GetInstance().GetRenderer();

	static const D3D12_HEAP_PROPERTIES DefaultHeapProps =
	{
		D3D12_HEAP_TYPE_DEFAULT,
		D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
		D3D12_MEMORY_POOL_UNKNOWN,
		0,
		0
	};

	auto& ObjectDescriptorHandles = currentFrameResources.ObjectCB.DescriptorHeapHandleGpu;
	for (uint32 i = 0; i < Entities.Count(); ++i)
	{
		if (!Entities[i]->RenderModel.Model)
		{
			continue;
		}
		const graphics::SRenderModel& model = *Entities[i]->RenderModel.Model;
		if (!model.VertexBufferGpu || !model.IndexBufferGpu)
		{
			continue;
		}

		if (i < ObjectDescriptorHandles.size())
		{
			CommandList->SetGraphicsRootDescriptorTable(
				render::constants::RootParam_ObjectCbv,
				ObjectDescriptorHandles[i]);
		}

		if (Entities[i]->bRayTraced)
		{}
		else // if ray-traced
		{
			{
				D3D12_INDEX_BUFFER_VIEW indexBufferView = {};
				indexBufferView.BufferLocation = model.IndexBufferGpu->GetGPUVirtualAddress();
				indexBufferView.SizeInBytes = model.Indices.Count() * sizeof(uint32);
				indexBufferView.Format = DXGI_FORMAT_R32_UINT;
				CommandList->IASetIndexBuffer(&indexBufferView);
			}
			{
				D3D12_VERTEX_BUFFER_VIEW vertexBufferViews[1] = {};
				vertexBufferViews[0].BufferLocation = model.VertexBufferGpu->GetGPUVirtualAddress();
				vertexBufferViews[0].SizeInBytes = model.Vertices.Count() * sizeof(graphics::SVertex);
				vertexBufferViews[0].StrideInBytes = sizeof(graphics::SVertex);
				CommandList->IASetVertexBuffers(0, 1, vertexBufferViews);
			}

			for (uint32 sectionIndex = 0; sectionIndex < model.Sections.Count(); ++sectionIndex)
			{
				const graphics::SRenderSection& section = model.Sections[sectionIndex];
				if (section.MaterialIndex < model.Materials.Count())
				{
					memory::TRefShared<graphics::SMaterial> material = model.Materials[section.MaterialIndex];
					D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = renderer->GetDefaultWhiteTextureGpu();
					if (material && material->bHasBaseColorTexture)
					{
						textureHandle = material->BaseColorTexture.GpuDescriptor;
					}

					CommandList->SetGraphicsRootDescriptorTable(
						render::constants::RootParam_BaseColorTexture,
						textureHandle);

					if (material)
					{
						ID3D12PipelineState* pipelineState = renderer->GetPipelineStateForMaterial(*material);
						if (pipelineState)
						{
							CommandList->SetPipelineState(pipelineState);
						}

						graphics::SFrameResources& frameResources = renderer->GetCurrentFrameResource();
						const auto& materialHandles = frameResources.MaterialCB.DescriptorHeapHandleGpu;
						if (material->RuntimeIndex < materialHandles.size())
						{
							CommandList->SetGraphicsRootDescriptorTable(
								render::constants::RootParam_MaterialCbv,
								materialHandles[material->RuntimeIndex]);
						}
					}
				}

				CommandList->DrawIndexedInstanced(
					section.IndexCount,
					1,
					section.IndexOffset,
					section.VertexOffset,
					0);
			}
		}
	}
}

void CWorld::InitializeRendering ()
{
	Renderer->BeginInitializationCommands();
	CreateAccelerationStructures();
	Renderer->InitializeRaytracingResources();
	Renderer->EndInitializationCommands();
}

void CWorld::CopyConstantData ()
{
	// TODO: ideally, CBs should already be stored in one array
	// TODO: use (when it's implemented) memory pool
	const uint32 entityCount = Entities.Count();
	Renderer->EnsureObjectConstantCapacity(entityCount);

	auto& currentFrameResources = Renderer->GetCurrentFrameResource();

	if (entityCount > 0u)
	{
		TArray<graphics::SObjectConstants> objectConstants;
		objectConstants.SetSizeUninitialized(entityCount);
		for (uint32 i = 0; i < entityCount; ++i)
		{
			objectConstants[i].World = Entities[i]->Transform.GetMatrix();
		}

		currentFrameResources.ObjectCB.CopyBunch(
			objectConstants.GetData(),
			objectConstants.Count(),
			currentFrameResources.UploadArena);
	}

	using namespace DirectX;

	graphics::SPassConstants passConstants;
	const auto [renderWidth, renderHeight] = GameInstance::GetInstance().GetWindow().GetWindowSize();

	const auto Camera = GameInstance::GetInstance().GetCamera();
	XMMATRIX view = Camera->GetViewMatrix();
	XMMATRIX projection = Camera->GetProjectionMatrix(
		math::PI_OVER_TWO, (float)renderWidth / renderHeight, 1.f, 1'000.f);

	XMMATRIX viewProj = XMMatrixMultiply(view, projection);

	auto viewDeterminant = XMMatrixDeterminant(view);
	XMMATRIX invView = XMMatrixInverse(&viewDeterminant, view);

	auto projectionDeterminant = XMMatrixDeterminant(projection);
	XMMATRIX invProj = XMMatrixInverse(&projectionDeterminant, projection);

	auto viewProjDeterminant = XMMatrixDeterminant(viewProj);
	XMMATRIX invViewProj = XMMatrixInverse(&viewProjDeterminant, viewProj);

	XMStoreFloat4x4(&passConstants.View, view);
	XMStoreFloat4x4(&passConstants.ViewInverse, invView);
	XMStoreFloat4x4(&passConstants.Projection, projection);
	XMStoreFloat4x4(&passConstants.ProjectionInverse, invProj);
	XMStoreFloat4x4(&passConstants.ViewProjection, viewProj);
	XMStoreFloat4x4(&passConstants.ViewProjectionInverse, invViewProj);
	passConstants.CameraPosition = math::ToDirectXCoordinates(Camera->Transform.GetTranslation());
	passConstants.RenderTargetSize = Vector2f(renderWidth, renderHeight);
	passConstants.RenderTargetSizeInverse = Vector2f(1.0f / renderWidth, 1.0f / renderHeight);
	passConstants.NearPlane = .1f;
	passConstants.FarPlane = 1'000.0f;
	passConstants.TotalTime = GameInstance::GetInstance().GetTime().GetTotalSeconds();
	passConstants.DeltaTime = GameInstance::GetInstance().GetTime().GetDeltaSeconds();

	currentFrameResources.PassCB.CopyBunch(&passConstants, 1u, currentFrameResources.UploadArena);
}
#endif

memory::TRefShared<CEntity> CWorld::SpawnEntity ()
{
	auto newEntity = memory::NewShared<CEntity>();
	Entities.Add(newEntity);
	return newEntity;
}

graphics::raytracing::SAccelerationStructureBuffers CWorld::CreateBottomLevelAS (
	const graphics::Comp_RenderModel& RenderModel)
{
	using namespace graphics;
	using namespace graphics::raytracing;

	CBottomLevelASGenerator bottomLevelAS;

	frt_assert(RenderModel.Model);
	const SRenderModel& model = *RenderModel.Model;

	// Adding all vertex buffers and not transforming their position.
	bottomLevelAS.AddVertexBuffer(
		model.VertexBufferGpu.Get(), 0, model.Vertices.Count(), sizeof(SVertex),
		model.IndexBufferGpu.Get(), 0, model.Indices.Count(),
		nullptr, 0);

	// The AS build requires some scratch space to store temporary information.
	// The amount of scratch memory is dependent on the scene complexity.
	uint64 scratchSizeInBytes = 0;
	// The final AS also needs to be stored in addition to the existing vertex
	// buffers. Its size is also dependent on the scene complexity.
	uint64 resultSizeInBytes = 0;

	ID3D12Device5* device = Renderer->GetDevice();

	bottomLevelAS.ComputeASBufferSizes(device, false, &scratchSizeInBytes, &resultSizeInBytes);

	// Once the sizes are obtained, the application is responsible for allocating
	// the necessary buffers. Since the entire generation will be done on the GPU,
	// we can directly allocate those on the default heap
	SAccelerationStructureBuffers buffers;
	buffers.Scratch = CreateBuffer(
		device, scratchSizeInBytes,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON,
		DefaultHeapProps);
	buffers.Result = CreateBuffer(
		device, resultSizeInBytes,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE,
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		DefaultHeapProps);
	if (buffers.Scratch)
	{
		buffers.Scratch->SetName(L"BLAS Scratch");
	}
	if (buffers.Result)
	{
		buffers.Result->SetName(L"BLAS Result");
	}

	ID3D12GraphicsCommandList4* commandList = Renderer->GetCommandList();

	// BLAS build expects geometry buffers in NON_PIXEL_SHADER_RESOURCE, and scratch/result in UAV/RTAS.
	D3D12_RESOURCE_BARRIER barriers[4] = {};
	uint32 barrierCount = 0;
	auto addTransition = [&] (
		ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before,
		D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER& barrier = barriers[barrierCount++];
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = resource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
	};

	addTransition(
		model.VertexBufferGpu.Get(),
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	addTransition(
		model.IndexBufferGpu.Get(),
		D3D12_RESOURCE_STATE_INDEX_BUFFER,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	addTransition(
		buffers.Scratch.Get(),
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (barrierCount > 0)
	{
		commandList->ResourceBarrier(barrierCount, barriers);
	}

	// Build the acceleration structure. Note that this call integrates a barrier
	// on the generated AS, so that it can be used to compute a top-level AS right
	// after this method.
	bottomLevelAS.Generate(
		commandList, buffers.Scratch.Get(),
		buffers.Result.Get(), false, nullptr);

	// Restore geometry buffers for rasterization.
	barrierCount = 0;
	addTransition(
		model.VertexBufferGpu.Get(),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
	addTransition(
		model.IndexBufferGpu.Get(),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_INDEX_BUFFER);
	commandList->ResourceBarrier(barrierCount, barriers);

	return buffers;
}

void CWorld::CreateTopLevelAS (const TArray<std::pair<ID3D12Resource*, DirectX::XMMATRIX>>& Instances)
{
	using namespace graphics;
	using namespace graphics::raytracing;

	TopLevelASGenerator = {};

	for (uint32 i = 0; i < Instances.Count(); i++)
	{
		TopLevelASGenerator.AddInstance(Instances[i].first, Instances[i].second, i, 0u);
	}

	ID3D12Device5* device = Renderer->GetDevice();

	// As for the bottom-level AS, the building the AS requires some scratch space
	// to store temporary data in addition to the actual AS. In the case of the
	// top-level AS, the instance descriptors also need to be stored in GPU
	// memory. This call outputs the memory requirements for each (scratch,
	// results, instance descriptors) so that the application can allocate the
	// corresponding memory
	uint64 scratchSize, resultSize, instanceDescsSize;


	TopLevelASGenerator.ComputeASBufferSizes(
		device, true, &scratchSize,
		&resultSize, &instanceDescsSize);

	// Create the scratch and result buffers. Since the build is all done on GPU,
	// those can be allocated on the default heap
	TopLevelASBuffers.Scratch = CreateBuffer(
		device, scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COMMON,
		DefaultHeapProps);
	TopLevelASBuffers.Result = CreateBuffer(
		device, resultSize,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE,
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		DefaultHeapProps);

	// The buffer describing the instances: ID, shader binding information,
	// matrices ... Those will be copied into the buffer by the helper through
	// mapping, so the buffer has to be allocated on the upload heap.
	TopLevelASBuffers.InstanceDesc = CreateBuffer(
		device, instanceDescsSize, D3D12_RESOURCE_FLAG_NONE,
		D3D12_RESOURCE_STATE_GENERIC_READ, UploadHeapProps);
	if (TopLevelASBuffers.Scratch)
	{
		TopLevelASBuffers.Scratch->SetName(L"TLAS Scratch");
	}
	if (TopLevelASBuffers.Result)
	{
		TopLevelASBuffers.Result->SetName(L"TLAS Result");
	}
	if (TopLevelASBuffers.InstanceDesc)
	{
		TopLevelASBuffers.InstanceDesc->SetName(L"TLAS InstanceDesc");
	}

	ID3D12GraphicsCommandList4* commandList = Renderer->GetCommandList();
	D3D12_RESOURCE_BARRIER barriers[2] = {};
	uint32 barrierCount = 0;
	auto addTransition = [&] (
		ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before,
		D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER& barrier = barriers[barrierCount++];
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = resource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
	};

	addTransition(
		TopLevelASBuffers.Scratch.Get(),
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	commandList->ResourceBarrier(barrierCount, barriers);

	// After all the buffers are allocated, or if only an update is required, we
	// can build the acceleration structure. Note that in the case of the update
	// we also pass the existing AS as the 'previous' AS, so that it can be
	// refitted in place.
	TopLevelASGenerator.Generate(
		commandList,
		TopLevelASBuffers.Scratch.Get(),
		TopLevelASBuffers.Result.Get(),
		TopLevelASBuffers.InstanceDesc.Get());
}

// definitely should be inside Renderer
void CWorld::CreateAccelerationStructures ()
{
	ID3D12Device5* device = Renderer->GetDevice();
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) ||
		options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
	{
		// Raytracing not supported on this device; skip AS build.
		return;
	}
	if (Entities.IsEmpty() || !Entities[2] || !Entities[2]->RenderModel.Model)
	{
		return;
	}

	graphics::raytracing::SAccelerationStructureBuffers bottomLevelBuffers =
		CreateBottomLevelAS(Entities[2]->RenderModel);

	// Just one instance for now
	// Instances = { { bottomLevelBuffers.Result.Get(), DirectX::XMLoadFloat4x4(&Entities[2]->Transform.GetMatrix()) } };
	Instances = { { bottomLevelBuffers.Result.Get(), DirectX::XMMatrixIdentity() } };
	CreateTopLevelAS(Instances);

	// Store the AS buffers. The rest of the buffers will be released once we exit
	// the function
	BottomLevelAS = bottomLevelBuffers.Result;

	Renderer->TopLevelASBuffers = TopLevelASBuffers;
}
