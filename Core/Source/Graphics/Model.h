#pragma once

#include <string>

#include "Core.h"
#include "Mesh.h"
#include "Containers/Array.h"
#include "ECS/ComponentRegistry.h"
#include "Memory/Ref.h"
#include "Render/GraphicsCoreTypes.h"
#include "Render/Material.h"

struct ID3D12Resource;


namespace frt::graphics
{
struct SVertex;
class CMaterialLibrary;
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
	uint32 SourceMesh = 0u;     // importer bookkeeping
};

struct FRT_CORE_API SRenderModel
{
	TArray<SRenderSection> Sections;
	TArray<memory::TRefShared<SMaterial>> Materials;

	TArray<SVertex> Vertices;
	TArray<uint32> Indices;

	std::string SourcePath;

	// Bumped whenever Vertices/Indices/Sections change; the GPU scene rebuilds the model's
	// buffers and BLAS when it sees a new value. CPU data is the source of truth.
	uint32 GeometryVersion = 1u;

	// Never repeats within the process - a copy or an assigned-to model gets a new one. The
	// GPU scene caches models by address, and a model freed with a dropped world (the editor's
	// Stop) can be followed by a different one at the same address.
	struct FRT_CORE_API SIdentity
	{
		SIdentity ();
		SIdentity (const SIdentity&);
		SIdentity& operator= (const SIdentity&);
		uint64 Value;
	};
	SIdentity Identity;

	// Materials resolve through MaterialLibrary (<model dir>/<model name>_mat<N>.frtmat.yml).
	static SRenderModel LoadFromFile (const std::string& Filename, const std::string& TexturePath, CMaterialLibrary& MaterialLibrary);
	static SRenderModel FromMesh (SMesh&& Mesh, memory::TRefShared<SMaterial> Material = nullptr);
};

struct FRT_CORE_API Comp_RenderModel
{
	memory::TRefShared<SRenderModel> Model;

	// When false the model is skipped by both raster draw and AS build — used for
	// portal visualization quads so they can be toggled off and not occlude rays.
	bool bVisible = true;

	// TODO: material overrides, bone pose, per-instance params.
};
}

FRT_DECLARE_COMPONENT_NAMED(frt::graphics::Comp_RenderModel, "Comp_RenderModel");
