#pragma once

#include <DirectXMath.h>

#include <unordered_map>
#include <vector>

#include "Graphics/Render/GraphicsCoreTypes.h"
#include "Graphics/Render/RenderContext.h"
#include "Graphics/Render/Scene/TextureManager.h"
#include "Shared/SceneData.h"


namespace frt::graphics
{
struct SRenderModel;
struct SMaterial;

// One TLAS instance as described by the game side for this frame.
struct SSceneInstance
{
	const SRenderModel* Model = nullptr;
	DirectX::XMFLOAT3X4 ObjectToWorld = {};   // render space, D3D12 instance layout (rows of the 3x4)
	uint64 Key = 0u;                          // stable identity, used to find last frame's transform
	bool bHelper = false;                     // debug geometry: camera rays only, not lit/shadowing
};

struct SSceneSky
{
	SSkyConstants Constants;
	float SunAngularRadius = 0.004654f;       // radians (~0.267 deg, the real sun)
	bool bSunEnabled = true;
	bool bSkyEnabled = true;
};

struct SSceneStats
{
	uint32 Instances = 0u;
	uint32 Models = 0u;
	uint64 Triangles = 0u;
	uint32 Materials = 0u;
	uint32 AnalyticLights = 0u;
	uint32 EmissiveTriangles = 0u;
	uint64 BlasBytes = 0u;
	uint32 BlasCompacted = 0u;
	uint32 TexturesResident = 0u;
	uint32 TexturesPending = 0u;
	uint64 TextureBytes = 0u;
};

// Raster preview draw (one per instance x section).
struct SSceneDraw
{
	D3D12_VERTEX_BUFFER_VIEW VertexBuffer = {};
	D3D12_INDEX_BUFFER_VIEW IndexBuffer = {};
	uint32 InstanceIndex = 0u;
	uint32 GeometryIndex = 0u;     // global SGeometryData index
	uint32 IndexCount = 0u;
	uint32 IndexOffset = 0u;
	int32 VertexOffset = 0;
	bool bAlphaBlend = false;
	bool bAlphaTested = false;     // alpha-masked material: depth-only passes must run the alpha test
	bool bHelper = false;          // debug geometry (portal quads): seen by the camera, casts no shadow
	DirectX::XMFLOAT3 BoundsMin = {};   // world-space AABB of the section (culling)
	DirectX::XMFLOAT3 BoundsMax = {};
};

// Raster preview stand-in for an emissive instance: a sphere light of the same power and area,
// centred on the emitting surface (exact for emissive spheres). The path tracer samples the
// emissive triangles themselves; a rasterizer cannot afford that per pixel.
struct SRasterEmitter
{
	DirectX::XMFLOAT3 Position = {};
	float Radius = 0.0f;
	DirectX::XMFLOAT3 Radiance = {};   // mean emitted radiance over the emitting area
	float Power = 0.0f;                // luminance flux, the ranking key
	uint32 InstanceIndex = 0u;         // the emitter itself, left out of its own shadow map
};

// Energy balance of an enclosed scene (one that declares sky portals), the raster preview's
// substitute for global illumination: the "ambient term" of progressive radiosity (Cohen et al.
// 1988). Everything that enters through the portals or is emitted inside bounces until it is
// absorbed or leaves through a portal; spread evenly over the enclosure, the interreflected
// light is a uniform radiance Flux * Albedo / (1 - Albedo) / (pi * Area) per channel.
struct SRasterAmbient
{
	bool bEnclosed = false;
	DirectX::XMFLOAT3 Radiance = {};   // uniform indirect radiance inside the enclosure
	DirectX::XMFLOAT3 Albedo = {};     // area-weighted mean reflectance (portal openings count as 0)
	DirectX::XMFLOAT3 Flux = {};       // power entering through the portals + emitted inside (W)
	float Area = 0.0f;                 // surface area, portal openings included (m^2)
	// The portals with their normals turned toward the interior. Which side is inside is taken
	// from the geometry, not from the authored normal (the path tracer only uses its line): the
	// side of the opening that holds the area-weighted centroid of the scene's surfaces.
	std::vector<gpu::SPortalData> Portals;
};

/**
 * GPU-side mirror of the scene: everything the path tracer and the raster preview read.
 *
 * The game describes the scene every frame (instances, lights, portals, sky). Update() turns
 * that into GPU tables, creating per-model data lazily from the CPU arrays of SRenderModel:
 *   - a compact ray tracing vertex stream (28 B) + the index buffer, raster vertex buffer,
 *   - one BLAS per model (PREFER_FAST_TRACE | ALLOW_COMPACTION, compacted two frames later),
 *   - material table entries with streamed textures,
 *   - emissive triangles registered as lights, a Walker alias table over all lights,
 *   - the baked sky environment + its luminance pyramid when sky parameters change,
 *   - a TLAS rebuilt every frame (cheap, best traversal quality; no refit drift).
 * Nothing the GPU may still read is released early: replaced resources go through the
 * device's deferred-release queue.
 */
class FRT_CORE_API CGpuScene
{
public:
	CGpuScene () = default;
	CGpuScene (const CGpuScene&) = delete;
	CGpuScene& operator= (const CGpuScene&) = delete;

	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines);
	void Shutdown ();

	// ---- frame description ----
	void BeginFrame ();
	void AddInstance (const SSceneInstance& Instance);
	void AddLight (const gpu::SLightData& Light);
	void AddPortal (const gpu::SPortalData& Portal);
	void SetSky (const SSceneSky& Sky);

	// Records uploads, AS builds and bakes. Returns true if anything that changes the image
	// (besides the camera) changed since last frame: progressive accumulation must restart.
	bool Update (CRenderContext& Context);

	void FillFrameConstants (gpu::SFrameConstants& Frame) const;
	D3D12_GPU_VIRTUAL_ADDRESS GetTlasAddress () const;
	const std::vector<SSceneDraw>& GetDraws () const { return Draws; }
	const SSceneStats& GetStats () const { return Stats; }
	CTextureManager& GetTextures () { return Textures; }
	// Nothing is loading: the scene was described at least once and every texture it requested
	// is resident (or failed to load).
	bool IsStreamingIdle () const { return FrameNumber > 0u && Textures.GetPendingCount() == 0u; }
	float GetSceneRadius () const { return SceneRadius; }
	DirectX::XMFLOAT3 GetSceneCenter () const { return SceneCenter; }
	DirectX::XMFLOAT3 GetSceneBoundsMin () const { return SceneMin; }
	DirectX::XMFLOAT3 GetSceneBoundsMax () const { return SceneMax; }

	// ---- raster preview inputs (no ray tracing needed) ----
	const std::vector<SRasterEmitter>& GetRasterEmitters () const { return RasterEmitters; }
	const SRasterAmbient& GetRasterAmbient () const { return RasterAmbient; }
	// Sky radiance pre-integrated against GGX lobes: Texture3D, xy = equal-area octahedral
	// direction, z = perceptual roughness (slice i = i / (SkyPrefilterSlices - 1)).
	uint32 GetSkyPrefilteredSrv () const { return SkyPrefiltered.Srv; }
	// Cosine-convolved sky: irradiance E(n) for a surface facing n (equal-area octahedral, RGBA16F).
	uint32 GetSkyIrradianceSrv () const { return SkyIrradiance.Srv; }
	static constexpr uint32 SkyPrefilterSize = 128u;
	static constexpr uint32 SkyPrefilterSlices = 8u;
	static constexpr uint32 SkyIrradianceSize = 32u;

	// Light selection: weight sun and sky by the power entering through the portals (closed
	// interiors) instead of over the scene disk. Changes noise only, never the expectation.
	bool bPortalWeightedLightSelection = true;

private:
	struct SModelEntry
	{
		uint64 ModelIdentity = 0u;               // SRenderModel::Identity: the address may be reused
		uint32 GeometryVersion = 0u;
		uint64 OpacitySignature = 0u;
		rhi::SGpuBuffer RtVertices;
		rhi::SGpuBuffer RasterVertices;
		rhi::SGpuBuffer Indices;
		rhi::SGpuBuffer Blas;
		rhi::SGpuBuffer CompactedSizeQuery;      // default heap UAV (postbuild info target)
		rhi::SGpuBuffer CompactedSizeReadback;
		uint64 CompactionRequestFrame = 0u;
		bool bCompacted = false;
		bool bCompactionQueried = false;
		uint32 GeometryBase = 0u;                // into the geometry table
		uint32 SectionCount = 0u;
		uint64 TriangleCount = 0u;
		uint32 EmissiveTriangleCount = 0u;       // per instance of this model
		DirectX::XMFLOAT3 BoundsMin = {};
		DirectX::XMFLOAT3 BoundsMax = {};
		uint64 LastUsedFrame = 0u;
		// Object-space area and area-weighted centroid per section (raster energy balance).
		std::vector<float> SectionArea;
		std::vector<DirectX::XMFLOAT3> SectionCentroid;
		std::vector<DirectX::XMFLOAT3> SectionBoundsMin;
		std::vector<DirectX::XMFLOAT3> SectionBoundsMax;
	};

	struct SMaterialEntry
	{
		uint32 Index = 0u;
		uint32 Version = 0u;
		CTextureManager::HTexture BaseColor = CTextureManager::InvalidHandle;
		CTextureManager::HTexture Normal = CTextureManager::InvalidHandle;
		CTextureManager::HTexture MetallicRoughness = CTextureManager::InvalidHandle;
		CTextureManager::HTexture Emissive = CTextureManager::InvalidHandle;
		bool bTexturesResolved = false;
	};

	SModelEntry& EnsureModel (CRenderContext& Context, const SRenderModel& Model, bool& bOutChanged);
	void BuildBlas (CRenderContext& Context, const SRenderModel& Model, SModelEntry& Entry);
	void UpdateCompaction (CRenderContext& Context);
	uint32 EnsureMaterial (const SMaterial* Material, bool& bOutChanged);
	void WriteMaterialData (const SMaterial& Material, SMaterialEntry& Entry);
	void UploadTable (CRenderContext& Context, rhi::SGpuBuffer& Buffer, const void* Data, uint64 Size, uint32 Stride, FStringView Name);
	void BuildTlas (CRenderContext& Context);
	void BakeSky (CRenderContext& Context);
	void BakeEnergyLut (CRenderContext& Context);
	void BuildLightTables (CRenderContext& Context);
	void UpdateRasterLighting ();

	rhi::CDevice* Device = nullptr;
	rhi::CPipelineLibrary* Pipelines = nullptr;
	CTextureManager Textures;

	// Frame description.
	std::vector<SSceneInstance> FrameInstances;
	std::vector<gpu::SLightData> FrameLights;
	std::vector<gpu::SPortalData> FramePortals;
	SSceneSky FrameSky;

	// Persistent GPU data.
	std::unordered_map<const SRenderModel*, SModelEntry> Models;
	std::unordered_map<const SMaterial*, SMaterialEntry> MaterialEntries;
	std::vector<gpu::SMaterialData> MaterialTable;
	std::vector<gpu::SGeometryData> GeometryTable;
	std::vector<gpu::SInstanceData> InstanceTable;
	std::vector<gpu::SEmissiveTriangle> EmissiveTriangles;
	std::vector<gpu::SLightData> LightTable;
	std::vector<gpu::SAliasEntry> AliasTable;
	std::unordered_map<uint64, DirectX::XMFLOAT3X4> PreviousTransforms;

	rhi::SGpuBuffer MaterialBuffer;
	rhi::SGpuBuffer GeometryBuffer;
	rhi::SGpuBuffer InstanceBuffer;
	rhi::SGpuBuffer LightBuffer;
	rhi::SGpuBuffer EmissiveBuffer;
	rhi::SGpuBuffer AliasBuffer;
	rhi::SGpuBuffer PortalBuffer;

	rhi::SGpuBuffer Tlas[rhi::MaxFramesInFlight];
	rhi::SGpuBuffer TlasScratch;
	rhi::SGpuBuffer BlasScratch;
	uint32 CurrentTlas = 0u;

	rhi::SGpuTexture SkyEnvironment;
	rhi::SGpuTexture SkyPyramid;
	rhi::SGpuTexture EnergyLut;
	rhi::HPipeline SkyBakePipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline SkyPyramidPipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline EnergyLutPipeline = rhi::InvalidPipelineHandle;
	rhi::SGpuTexture SkyPrefiltered;
	rhi::SGpuTexture SkyIrradiance;
	rhi::HPipeline SkyPrefilterPipeline = rhi::InvalidPipelineHandle;
	rhi::HPipeline SkyIrradiancePipeline = rhi::InvalidPipelineHandle;
	std::vector<SRasterEmitter> RasterEmitters;
	SRasterAmbient RasterAmbient;
	float LoggedAmbientLuminance = -1.0f;
	uint64 SkyHash = 0u;
	bool bEnergyLutBaked = false;
	float SkyAverageLuminance = 0.0f;
	static constexpr uint32 SkyEnvironmentSize = 256u;

	uint32 SunLightIndex = FRT_INVALID_INDEX;
	uint32 SkyLightIndex = FRT_INVALID_INDEX;
	uint64 GeometryLayoutHash = 0u;
	uint64 EmissiveLayoutHash = 0u;
	uint64 LightHash = 0u;
	uint64 InstanceHash = 0u;
	bool bMaterialTableDirty = true;
	bool bGeometryTableDirty = true;
	float SceneRadius = 10.0f;
	DirectX::XMFLOAT3 SceneCenter = {};
	DirectX::XMFLOAT3 SceneMin = { -10.0f, -10.0f, -10.0f };
	DirectX::XMFLOAT3 SceneMax = { 10.0f, 10.0f, 10.0f };

	std::vector<SSceneDraw> Draws;
	SSceneStats Stats;
	uint64 FrameNumber = 0u;
};
}
