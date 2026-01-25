#pragma once

#include <string>

#include "Core.h"
#include "Mesh.h"
#include "Containers/Array.h"
#include "Memory/Ref.h"
#include "Render/ConstantBuffer.h"
#include "Render/GraphicsCoreTypes.h"
#include "Render/Material.h"

struct ID3D12Resource;


namespace frt::graphics
{
struct SVertex;
struct STexture;
}


namespace frt::graphics
{
struct FRT_CORE_API SRenderSection
{
	uint32 IndexOffset = 0u;
	uint32 IndexCount = 0u;
	uint32 VertexOffset = 0u;
	uint32 VertexCount = 0u;
	uint32 MaterialIndex = 0u;
};

struct FRT_CORE_API SRenderModel
{
	TArray<SRenderSection> Sections;
	TArray<memory::TRefShared<SMaterial>> Materials;

	TArray<SVertex> Vertices;
	TArray<uint32> Indices;

	ComPtr<ID3D12Resource> VertexBufferGpu = nullptr;
	ComPtr<ID3D12Resource> IndexBufferGpu = nullptr;

	static SRenderModel LoadFromFile (const std::string& Filename, const std::string& TexturePath);
	static SRenderModel FromMesh (SMesh&& Mesh, memory::TRefShared<SMaterial> Material = nullptr);
};

struct FRT_CORE_API Comp_RenderModel
{
	memory::TRefShared<SRenderModel> Model;

	// TODO: material overrides, bone pose, per-instance params.
};
}
