#include "Sys_MeshRenderer.h"

#include <cstring>
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


namespace
{
bool IsRaytracingSupported (ID3D12Device5* Device)
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
	return SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))
			&& options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
}

bool AreMatricesEqual (const DirectX::XMFLOAT4X4& A, const DirectX::XMFLOAT4X4& B)
{
	return std::memcmp(&A, &B, sizeof(DirectX::XMFLOAT4X4)) == 0;
}
}

#if !defined(FRT_HEADLESS)
Sys_MeshRenderer::Sys_MeshRenderer (memory::TRefWeak<graphics::CRenderer> InRenderer)
	: Renderer(InRenderer)
{}
#else
Sys_MeshRenderer::CWorld ()
{}
#endif

SFlags<EUpdatePhase>& Sys_MeshRenderer::GetPhases ()
{
	return SFlags<EUpdatePhase>()
			.AddFlags(EUpdatePhase::Update)
			.AddFlags(EUpdatePhase::Draw);
}

void Sys_MeshRenderer::Tick (float DeltaSeconds)
{
#ifndef FRT_HEADLESS
	CopyConstantData();
#endif
}

#ifndef FRT_HEADLESS
void Sys_MeshRenderer::Present (float DeltaSeconds, ID3D12GraphicsCommandList4* CommandList)
{
	auto& currentFrameResources = Renderer->GetCurrentFrameResource();

	// TODO: assign stable material indices in MaterialLibrary and update constants only when dirty.
	std::unordered_map<const graphics::SMaterial*, uint32> materialIndices;
	TArray<graphics::SMaterialConstants> materialConstants;
	TArray<graphics::CRenderer::SRaytracingMaterialTextureSet> rtMaterialTextureSets;
	TArray<graphics::CRenderer::SRaytracingHitGroupEntry> rtHitGroupEntries;

	for (auto RenderModel : RenderModels)
	{
		graphics::SRenderModel& model = *RenderModel->Model;
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
				constants.Roughness = material->Roughness;
				constants.Metallic = material->Metallic;
				constants.Emissive = material->Emissive;
				constants.EmissiveIntensity = material->EmissiveIntensity;

				constants.Flags = material->Flags.Flags;
				for (uint32 textureIndex = 0; textureIndex < render::constants::RootMaterialTextureCount; ++
					textureIndex)
				{
					constants.TextureIndices[textureIndex] = render::constants::MaterialTextureIndexInvalid;
				}

				auto& textureSet = rtMaterialTextureSets.Add();
				if (material->bHasBaseColorTexture && material->BaseColorTexture.GpuTexture)
				{
					constants.TextureIndices[render::constants::MaterialTextureSlot_BaseColor] =
						render::constants::MaterialTextureSlot_BaseColor;
					textureSet.Textures[render::constants::MaterialTextureSlot_BaseColor] =
						material->BaseColorTexture.GpuTexture;
				}

				material->RuntimeIndex = materialIndex;
			}
			else
			{
				material->RuntimeIndex = it->second;
			}
		}
	}

	Renderer->EnsureMaterialConstantCapacity(materialConstants.Count());
	Renderer->SetRaytracingMaterialTextureSets(rtMaterialTextureSets);

	if (!AsEntities.IsEmpty() && (AsEntities.Count() == AsModels.Count()))
	{
		rtHitGroupEntries.Reset(AsEntities.Count());
		for (uint32 i = 0; i < AsEntities.Count(); ++i)
		{
			const CEntity* entity = AsEntities[i];
			const graphics::SRenderModel* model = AsModels[i];
			frt_assert(entity && model && model->VertexBufferGpu && model->IndexBufferGpu);

			uint32 materialIndex = 0u;
			if (!model->Sections.IsEmpty())
			{
				const graphics::SRenderSection& section = model->Sections[0];
				if (section.MaterialIndex < model->Materials.Count())
				{
					const graphics::SMaterial* material =
						model->Materials[section.MaterialIndex].GetRawIgnoringLifetime();
					if (material)
					{
						materialIndex = material->RuntimeIndex;
					}
				}
			}

			auto& entry = rtHitGroupEntries.Add();
			entry.MaterialIndex = materialIndex;
			entry.VertexBuffer = model->VertexBufferGpu.Get();
			entry.IndexBuffer = model->IndexBufferGpu.Get();
		}
	}

	Renderer->SetRaytracingHitGroupEntries(rtHitGroupEntries);
	if (!materialConstants.IsEmpty())
	{
		currentFrameResources.MaterialCB.CopyBunch(
			materialConstants.GetData(),
			materialConstants.Count(),
			currentFrameResources.UploadArena);
	}

	currentFrameResources.ObjectCB.Upload(currentFrameResources.UploadArena, CommandList);
	currentFrameResources.MaterialCB.Upload(currentFrameResources.UploadArena, CommandList);

	if (!Renderer->ShouldRenderRaster())
	{
		return;
	}

	if (!currentFrameResources.PassCB.DescriptorHeapHandleGpu.empty())
	{
		CommandList->SetGraphicsRootDescriptorTable(
			render::constants::RootParam_PassCbv,
			currentFrameResources.PassCB.DescriptorHeapHandleGpu[0]);
	}

	memory::TRefWeak<graphics::CRenderer> renderer = GameInstance::GetInstance().GetRenderer();

	auto& ObjectDescriptorHandles = currentFrameResources.ObjectCB.DescriptorHeapHandleGpu;
	for (uint32 i = 0; i < RenderModels.Count(); ++i)
	{
		const graphics::SRenderModel& model = *RenderModels[i]->Model;
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

void Sys_MeshRenderer::InitializeRendering ()
{
	Renderer->BeginInitializationCommands();
	CreateAccelerationStructures();
	Renderer->InitializeRaytracingResources();
	Renderer->EndInitializationCommands();
}

void Sys_MeshRenderer::CopyConstantData ()
{
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
	passConstants.FrameIndex = static_cast<uint32>(GameInstance::GetInstance().GetFrameCount());
	// RaytracingSampleCount left at its default (32); set here to make it visible and easy to tune
	// passConstants.RaytracingSampleCount = 32u;

	// ── Accumulation frame counter ────────────────────────────────────────
	// Reset on any scene change: camera movement, object transforms, or topology.
	// bAccumulationDirty is owned by CWorldScene; renderers and systems set it
	// there so all consumers share one authoritative signal.
	CWorldScene& scene = GameInstance::GetInstance().GetWorldScene();
	{
		DirectX::XMFLOAT4X4 currentView;
		XMStoreFloat4x4(&currentView, view);
		if (!AreMatricesEqual(currentView, PrevCameraViewMatrix))
		{
			scene.bAccumulationDirty = true;
			PrevCameraViewMatrix = currentView;
		}
	}

	if (scene.bAccumulationDirty)
	{
		AccumulationFrameIndex = 0u;
		scene.bAccumulationDirty = false;
	}
	else
	{
		++AccumulationFrameIndex;
	}

	passConstants.AccumulationFrameIndex = AccumulationFrameIndex;

	auto& currentFrameResources = Renderer->GetCurrentFrameResource();
	currentFrameResources.PassCB.CopyBunch(&passConstants, 1u, currentFrameResources.UploadArena);
}

void Sys_MeshRenderer::UploadCB (ID3D12GraphicsCommandList4* CommandList)
{
	auto& currentFrameResources = Renderer->GetCurrentFrameResource();

	currentFrameResources.PassCB.Upload(currentFrameResources.UploadArena, CommandList);
}
#endif

memory::TRefShared<graphics::Comp_RenderModel> Sys_MeshRenderer::SpawnRenderModel ()
{
	memory::TRefShared<graphics::Comp_RenderModel> renderModel = memory::NewShared<graphics::Comp_RenderModel>();
	RenderModels.Add(renderModel);
	return renderModel;
}

// memory::TRefShared<CEntity> Sys_MeshRenderer::SpawnEntity ()
// {
// 	auto newEntity = memory::NewShared<CEntity>();
// 	Entities.Add(newEntity);
// #ifndef FRT_HEADLESS
// 	bAsTopologyDirty = true;
// #endif
// 	return newEntity;
// }

graphics::raytracing::SAccelerationStructureBuffers Sys_MeshRenderer::CreateBottomLevelAS (
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
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	addTransition(
		model.IndexBufferGpu.Get(),
		D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
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
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	addTransition(
		model.IndexBufferGpu.Get(),
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	commandList->ResourceBarrier(barrierCount, barriers);

	return buffers;
}

void Sys_MeshRenderer::CreateTopLevelAS (const TArray<SAccelerationInstance>& Instances, bool bUpdateOnly)
{
	using namespace graphics;
	using namespace graphics::raytracing;

	TopLevelASGenerator = {};

	for (uint32 i = 0; i < Instances.Count(); i++)
	{
		TopLevelASGenerator.AddInstance(
			Instances[i].BottomLevelAS,
			Instances[i].Transform,
			Instances[i].InstanceId,
			Instances[i].HitGroupIndex);
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

	const bool bHasValidBuffers = TopLevelASBuffers.Result && TopLevelASBuffers.InstanceDesc;
	const bool bCanUpdateOnly = bUpdateOnly && bHasValidBuffers;
	if (!bCanUpdateOnly)
	{
		TopLevelASBuffers.Result = CreateBuffer(
			device, resultSize,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			DefaultHeapProps);
	}

	// Scratch and descriptor buffers can be recreated if required by size.
	const uint64 currentScratchSize = TopLevelASBuffers.Scratch ? TopLevelASBuffers.Scratch->GetDesc().Width : 0u;
	bool bScratchBufferRecreated = false;
	if (!TopLevelASBuffers.Scratch || currentScratchSize < scratchSize)
	{
		TopLevelASBuffers.Scratch = CreateBuffer(
			device, scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_COMMON,
			DefaultHeapProps);
		bScratchBufferRecreated = true;
	}

	const uint64 currentDescSize = TopLevelASBuffers.InstanceDesc
										? TopLevelASBuffers.InstanceDesc->GetDesc().Width
										: 0u;
	if (instanceDescsSize > 0u && (!TopLevelASBuffers.InstanceDesc || currentDescSize < instanceDescsSize))
	{
		TopLevelASBuffers.InstanceDesc = CreateBuffer(
			device, instanceDescsSize, D3D12_RESOURCE_FLAG_NONE,
			D3D12_RESOURCE_STATE_GENERIC_READ, UploadHeapProps);
	}

#ifndef RELEASE
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
#endif

	ID3D12GraphicsCommandList4* commandList = Renderer->GetCommandList();

	if (bScratchBufferRecreated)
	{
		D3D12_RESOURCE_BARRIER scratchBarrier = {};
		scratchBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		scratchBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		scratchBarrier.Transition.pResource = TopLevelASBuffers.Scratch.Get();
		scratchBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		scratchBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		scratchBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		commandList->ResourceBarrier(1, &scratchBarrier);
	}

	// After all the buffers are allocated, or if only an update is required, we
	// can build the acceleration structure. Note that in the case of the update
	// we also pass the existing AS as the 'previous' AS, so that it can be
	// refitted in place.
	TopLevelASGenerator.Generate(
		commandList,
		TopLevelASBuffers.Scratch.Get(),
		TopLevelASBuffers.Result.Get(),
		TopLevelASBuffers.InstanceDesc.Get(),
		bCanUpdateOnly,
		bCanUpdateOnly ? TopLevelASBuffers.Result.Get() : nullptr);
}

// definitely should be inside Renderer
void Sys_MeshRenderer::CreateAccelerationStructures ()
{
	ID3D12Device5* device = Renderer->GetDevice();
	if (!bRaytracingSupportChecked)
	{
		bRaytracingSupported = IsRaytracingSupported(device);
		bRaytracingSupportChecked = true;
	}
	if (!bRaytracingSupported)
	{
		return;
	}

	CWorldScene& scene = GameInstance::GetInstance().GetWorldScene();
	const auto& sceneEntities = scene.GetEntities();

	struct SBuildEntry
	{
		const CEntity* Entity = nullptr;
		const graphics::SRenderModel* Model = nullptr;
		DirectX::XMFLOAT3X4 Transform = {};
	};

	std::unordered_map<graphics::SMaterial*, uint32> materialIndices;
	TArray<SBuildEntry> buildEntries;
	buildEntries.SetCapacity(sceneEntities.Count());

	for (uint32 i = 0; i < sceneEntities.Count(); ++i)
	{
		const CEntity* entity = sceneEntities[i].GetRawIgnoringLifetime();
		if (!entity || !entity->RenderModel->Model)
		{
			continue;
		}

		const graphics::SRenderModel& model = *entity->RenderModel->Model;
		for (const graphics::SRenderSection& section : model.Sections)
		{
			if (section.MaterialIndex >= model.Materials.Count())
			{
				continue;
			}

			graphics::SMaterial* material = const_cast<graphics::SMaterial*>(model.Materials[section.MaterialIndex].
				GetRawIgnoringLifetime());
			if (!material)
			{
				continue;
			}

			auto it = materialIndices.find(material);
			if (it == materialIndices.end())
			{
				const uint32 materialIndex = materialIndices.size();
				materialIndices.emplace(material, materialIndex);
				material->RuntimeIndex = materialIndex;
			}
			else
			{
				material->RuntimeIndex = it->second;
			}
		}

		SBuildEntry entry = {};
		entry.Entity = entity;
		entry.Model = &model;
		entry.Transform = entity->Transform.GetRaytracingTransform();
		buildEntries.Add(entry);
	}

	if (buildEntries.IsEmpty())
	{
		BottomLevelASs.Clear();
		TopLevelASBuffers = {};
		Renderer->TopLevelASBuffers = {};
		AsEntities.Clear();
		AsModels.Clear();
		AsTransforms.Clear();
		Instances.Clear();
		bAsInitialized = true;
		scene.bSceneTopologyDirty = false;
		return;
	}

	if (!materialIndices.empty())
	{
		Renderer->EnsureMaterialConstantCapacity(static_cast<uint32>(materialIndices.size()));
	}

	TArray<graphics::raytracing::SAccelerationStructureBuffers> bottomLevelBuffers;
	bottomLevelBuffers.SetCapacity(buildEntries.Count());

	for (const SBuildEntry& entry : buildEntries)
	{
		bottomLevelBuffers.Add(CreateBottomLevelAS(*entry.Entity->GetRenderModel()));
	}

	Instances.Reset(buildEntries.Count());
	AsEntities.Reset(buildEntries.Count());
	AsModels.Reset(buildEntries.Count());
	AsTransforms.Reset(buildEntries.Count());

	for (uint32 i = 0; i < buildEntries.Count(); ++i)
	{
		const SBuildEntry& entry = buildEntries[i];
		Instances.Add({ bottomLevelBuffers[i].Result.Get(), entry.Transform, i, i * 2u });
		AsEntities.Add(entry.Entity);
		AsModels.Add(entry.Model);
		AsTransforms.Add(entry.Entity->Transform.GetMatrix());
	}

	CreateTopLevelAS(Instances, false);

	BottomLevelASs.Reset(bottomLevelBuffers.Count());
	for (const auto& buffer : bottomLevelBuffers)
	{
		BottomLevelASs.Add(buffer.Result);
	}

	Renderer->TopLevelASBuffers = TopLevelASBuffers;
	bAsInitialized = true;
	scene.bSceneTopologyDirty = false;
}

void Sys_MeshRenderer::UpdateAccelerationStructures ()
{
	if (!Renderer->ShouldRenderRaytracing())
	{
		return;
	}

	CWorldScene& scene = GameInstance::GetInstance().GetWorldScene();
	const auto& sceneEntities = scene.GetEntities();

	if (!bAsInitialized || scene.bSceneTopologyDirty)
	{
		CreateAccelerationStructures();
		if (TopLevelASBuffers.Result)
		{
			Renderer->InitializeRaytracingResources();
		}
		scene.bAccumulationDirty = true;
		return;
	}

	if (AsEntities.Count() != AsModels.Count() || AsEntities.Count() != AsTransforms.Count())
	{
		scene.bSceneTopologyDirty = true;
		CreateAccelerationStructures();
		if (TopLevelASBuffers.Result)
		{
			Renderer->InitializeRaytracingResources();
		}
		return;
	}

	uint32 trackedIndex = 0u;
	bool bTopologyChanged = false;
	bool bInstanceDataChanged = false;

	for (uint32 i = 0; i < sceneEntities.Count(); ++i)
	{
		const CEntity* entity = sceneEntities[i].GetRawIgnoringLifetime();
		if (!entity || !entity->RenderModel->Model)
		{
			continue;
		}

		if (trackedIndex >= AsEntities.Count() ||
			AsEntities[trackedIndex] != entity ||
			AsModels[trackedIndex] != entity->RenderModel->Model.GetRawIgnoringLifetime())
		{
			bTopologyChanged = true;
			break;
		}

		DirectX::XMFLOAT4X4 currentTransform = entity->GetTransform().GetMatrix();
		if (!AreMatricesEqual(currentTransform, AsTransforms[trackedIndex]))
		{
			AsTransforms[trackedIndex] = currentTransform;
			Instances[trackedIndex].Transform = entity->GetTransform().GetRaytracingTransform();
			bInstanceDataChanged = true;
		}

		const uint32 expectedHitGroupIndex = trackedIndex * 2u;
		if (Instances[trackedIndex].HitGroupIndex != expectedHitGroupIndex)
		{
			Instances[trackedIndex].HitGroupIndex = expectedHitGroupIndex;
			bInstanceDataChanged = true;
		}

		++trackedIndex;
	}

	if (!bTopologyChanged && trackedIndex != AsEntities.Count())
	{
		bTopologyChanged = true;
	}

	if (bTopologyChanged)
	{
		scene.bSceneTopologyDirty = true;
		CreateAccelerationStructures();
		if (TopLevelASBuffers.Result)
		{
			Renderer->InitializeRaytracingResources();
		}
		scene.bAccumulationDirty = true;
		return;
	}

	if (!bInstanceDataChanged || Instances.IsEmpty())
	{
		return;
	}

	// Per-frame transform update (objects moving): reset accumulation so
	// the stale history isn't blended with the new object positions.
	CreateTopLevelAS(Instances, true);
	Renderer->TopLevelASBuffers = TopLevelASBuffers;
	scene.bAccumulationDirty = true;
}
