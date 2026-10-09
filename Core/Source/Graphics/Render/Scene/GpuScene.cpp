#include "Graphics/Render/Scene/GpuScene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Graphics/Model.h"
#include "Graphics/Render/Material.h"
#include "Profiler/Profiler.h"
#include "Shared/SkyBakeData.h"


namespace frt::graphics
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;

uint64 HashBytes (uint64 Hash, const void* Data, size_t Size)
{
	const uint8* bytes = static_cast<const uint8*>(Data);
	for (size_t i = 0; i < Size; ++i)
	{
		Hash ^= bytes[i];
		Hash *= 1099511628211ull;
	}
	return Hash;
}

template <typename T>
uint64 HashValue (uint64 Hash, const T& Value)
{
	return HashBytes(Hash, &Value, sizeof(T));
}

constexpr uint64 kHashSeed = 1469598103934665603ull;

float Luminance (float R, float G, float B)
{
	return 0.2126f * R + 0.7152f * G + 0.0722f * B;
}

// CPU mirror of Sky.hlsli EvalProceduralSky, for the alias-table weight of the sky.
DirectX::XMFLOAT3 EvalProceduralSkyCpu (const SSkyConstants& Sky, float DirY)
{
	auto smoothstep = [] (float e0, float e1, float x)
	{
		const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	};
	const float softness = (std::max)(Sky.HorizonSoftness, 1e-4f);
	const float skyT = smoothstep(-softness, softness, DirY);
	const float zenithT = std::clamp(DirY, 0.0f, 1.0f);
	const float sr = (Sky.SkyHorizonColor.R + (Sky.SkyZenithColor.R - Sky.SkyHorizonColor.R) * zenithT) * Sky.SkyIntensity;
	const float sg = (Sky.SkyHorizonColor.G + (Sky.SkyZenithColor.G - Sky.SkyHorizonColor.G) * zenithT) * Sky.SkyIntensity;
	const float sb = (Sky.SkyHorizonColor.B + (Sky.SkyZenithColor.B - Sky.SkyHorizonColor.B) * zenithT) * Sky.SkyIntensity;
	return {
		Sky.GroundColor.R + (sr - Sky.GroundColor.R) * skyT,
		Sky.GroundColor.G + (sg - Sky.GroundColor.G) * skyT,
		Sky.GroundColor.B + (sb - Sky.GroundColor.B) * skyT };
}

void PackOctahedral (float X, float Y, float Z, float& OutU, float& OutV)
{
	const float l1 = std::abs(X) + std::abs(Y) + std::abs(Z);
	if (l1 <= 0.0f)
	{
		OutU = 0.0f;
		OutV = 0.0f;
		return;
	}
	X /= l1;
	Y /= l1;
	if (Z < 0.0f)
	{
		const float ox = (1.0f - std::abs(Y)) * (X >= 0.0f ? 1.0f : -1.0f);
		const float oy = (1.0f - std::abs(X)) * (Y >= 0.0f ? 1.0f : -1.0f);
		X = ox;
		Y = oy;
	}
	OutU = X;
	OutV = Y;
}

uint32 PackSnorm2x16 (float U, float V)
{
	const int32 qu = static_cast<int32>(std::lround(std::clamp(U, -1.0f, 1.0f) * 32767.0f));
	const int32 qv = static_cast<int32>(std::lround(std::clamp(V, -1.0f, 1.0f) * 32767.0f));
	return (static_cast<uint32>(qu) & 0xFFFFu) | (static_cast<uint32>(qv) << 16u);
}

// Walker/Vose alias table. Weights must be >= 0; an all-zero set degrades to uniform.
void BuildAliasTable (const std::vector<float>& Weights, std::vector<gpu::SAliasEntry>& Out)
{
	const uint32 n = static_cast<uint32>(Weights.size());
	Out.assign(n, gpu::SAliasEntry{});
	if (n == 0u)
	{
		return;
	}
	double sum = 0.0;
	for (const float w : Weights)
	{
		sum += (std::max)(w, 0.0f);
	}
	std::vector<double> scaled(n);
	for (uint32 i = 0; i < n; ++i)
	{
		const double pmf = sum > 0.0 ? (std::max)(Weights[i], 0.0f) / sum : 1.0 / n;
		Out[i].Pmf = static_cast<float>(pmf);
		scaled[i] = pmf * n;
		Out[i].Alias = i;
		Out[i].Threshold = 1.0f;
	}
	std::vector<uint32> underfull;
	std::vector<uint32> overfull;
	underfull.reserve(n);
	overfull.reserve(n);
	for (uint32 i = 0; i < n; ++i)
	{
		(scaled[i] < 1.0 ? underfull : overfull).push_back(i);
	}
	while (!underfull.empty() && !overfull.empty())
	{
		const uint32 s = underfull.back();
		underfull.pop_back();
		const uint32 l = overfull.back();
		Out[s].Threshold = static_cast<float>(scaled[s]);
		Out[s].Alias = l;
		scaled[l] = (scaled[l] + scaled[s]) - 1.0;
		if (scaled[l] < 1.0)
		{
			overfull.pop_back();
			underfull.push_back(l);
		}
	}
	for (const uint32 i : overfull) { Out[i].Threshold = 1.0f; Out[i].Alias = i; }
	for (const uint32 i : underfull) { Out[i].Threshold = 1.0f; Out[i].Alias = i; }
}

void StoreRows (const DirectX::XMFLOAT3X4& M, gpu::float4 Out[3])
{
	for (uint32 r = 0; r < 3; ++r)
	{
		Out[r] = { M.m[r][0], M.m[r][1], M.m[r][2], M.m[r][3] };
	}
}

// Rows of inverse-transpose(3x3 part) and the determinant sign.
bool ComputeNormalMatrix (const DirectX::XMFLOAT3X4& M, gpu::float4 Out[3])
{
	const float a = M.m[0][0], b = M.m[0][1], c = M.m[0][2];
	const float d = M.m[1][0], e = M.m[1][1], f = M.m[1][2];
	const float g = M.m[2][0], h = M.m[2][1], i = M.m[2][2];
	const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	const float inv = std::abs(det) > 1e-20f ? 1.0f / det : 0.0f;
	// inverse-transpose = cofactor matrix / det
	Out[0] = { (e * i - f * h) * inv, -(d * i - f * g) * inv, (d * h - e * g) * inv, 0.0f };
	Out[1] = { -(b * i - c * h) * inv, (a * i - c * g) * inv, -(a * h - b * g) * inv, 0.0f };
	Out[2] = { (b * f - c * e) * inv, -(a * f - c * d) * inv, (a * e - b * d) * inv, 0.0f };
	return det < 0.0f;
}

DirectX::XMFLOAT3 TransformPoint (const DirectX::XMFLOAT3X4& M, float X, float Y, float Z)
{
	return {
		M.m[0][0] * X + M.m[0][1] * Y + M.m[0][2] * Z + M.m[0][3],
		M.m[1][0] * X + M.m[1][1] * Y + M.m[1][2] * Z + M.m[1][3],
		M.m[2][0] * X + M.m[2][1] * Y + M.m[2][2] * Z + M.m[2][3] };
}

bool IsMaterialNonOpaque (const SMaterial* Material)
{
	return Material && Material->AlphaMode != EAlphaMode::Opaque;
}
}


void CGpuScene::Initialize (rhi::CDevice& InDevice, rhi::CPipelineLibrary& InPipelines)
{
	Device = &InDevice;
	Pipelines = &InPipelines;
	Textures.Initialize(InDevice);

	SkyBakePipeline = Pipelines->CreateCompute("SkyBake", MakeShaderDesc("Scene/SkyBake.hlsl", "BakeEnvironment", "cs_6_6"));
	SkyPyramidPipeline = Pipelines->CreateCompute("SkyPyramid", MakeShaderDesc("Scene/SkyBake.hlsl", "BuildPyramid", "cs_6_6"));
	EnergyLutPipeline = Pipelines->CreateCompute("EnergyLut", MakeShaderDesc("Scene/EnergyLut.hlsl", "main", "cs_6_6"));

	rhi::STextureDesc env;
	env.Width = SkyEnvironmentSize;
	env.Height = SkyEnvironmentSize;
	env.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	env.bUav = true;
	env.Name = "SkyEnvironment";
	SkyEnvironment = Device->CreateTexture(env);

	rhi::STextureDesc pyramid;
	pyramid.Width = SkyEnvironmentSize;
	pyramid.Height = SkyEnvironmentSize;
	pyramid.MipLevels = static_cast<uint16>(rhi::ComputeMipCount(SkyEnvironmentSize, SkyEnvironmentSize));
	pyramid.Format = DXGI_FORMAT_R32_FLOAT;
	pyramid.bUav = true;
	pyramid.Name = "SkyLuminancePyramid";
	SkyPyramid = Device->CreateTexture(pyramid);

	rhi::STextureDesc lut;
	lut.Width = 32u;
	lut.Height = 32u;
	lut.Format = DXGI_FORMAT_R16G16_FLOAT;
	lut.bUav = true;
	lut.Name = "GgxEnergyLut";
	EnergyLut = Device->CreateTexture(lut);
}

void CGpuScene::Shutdown ()
{
	if (!Device)
	{
		return;
	}
	for (auto& [model, entry] : Models)
	{
		Device->Destroy(entry.RtVertices);
		Device->Destroy(entry.RasterVertices);
		Device->Destroy(entry.Indices);
		Device->Destroy(entry.Blas);
		Device->Destroy(entry.CompactedSizeQuery);
		Device->Destroy(entry.CompactedSizeReadback);
	}
	Models.clear();
	for (rhi::SGpuBuffer* buffer : { &MaterialBuffer, &GeometryBuffer, &InstanceBuffer, &LightBuffer, &EmissiveBuffer,
		&AliasBuffer, &PortalBuffer, &TlasScratch, &BlasScratch, &Tlas[0], &Tlas[1] })
	{
		Device->Destroy(*buffer);
	}
	Device->Destroy(SkyEnvironment);
	Device->Destroy(SkyPyramid);
	Device->Destroy(EnergyLut);
	Textures.Shutdown();
	Device = nullptr;
}

void CGpuScene::BeginFrame ()
{
	FrameInstances.clear();
	FrameLights.clear();
	FramePortals.clear();
}

void CGpuScene::AddInstance (const SSceneInstance& Instance)
{
	if (Instance.Model && !Instance.Model->Sections.IsEmpty() && !Instance.Model->Indices.IsEmpty())
	{
		FrameInstances.push_back(Instance);
	}
}

void CGpuScene::AddLight (const gpu::SLightData& Light)
{
	FrameLights.push_back(Light);
}

void CGpuScene::AddPortal (const gpu::SPortalData& Portal)
{
	FramePortals.push_back(Portal);
}

void CGpuScene::SetSky (const SSceneSky& Sky)
{
	FrameSky = Sky;
}

void CGpuScene::UploadTable (
	CRenderContext& Context,
	rhi::SGpuBuffer& Buffer,
	const void* Data,
	uint64 Size,
	uint32 Stride,
	FStringView Name)
{
	const uint64 required = (std::max<uint64>)(Size, Stride);
	if (!Buffer.IsValid() || Buffer.Size < required)
	{
		if (Buffer.IsValid())
		{
			Device->Destroy(Buffer);
		}
		uint64 capacity = 4096u;
		while (capacity < required)
		{
			capacity *= 2u;
		}
		rhi::SBufferDesc desc;
		desc.Size = capacity;
		desc.Stride = Stride;
		desc.SrvKind = rhi::EBufferViewKind::Structured;
		desc.Name = Name;
		Buffer = Device->CreateBuffer(desc);
	}
	if (Size > 0u)
	{
		Device->UploadBuffer(Context.CommandList, Buffer, Data, Size, 0u, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	else
	{
		rhi::CBarrierBatch barriers;
		barriers.Transition(Buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		barriers.Flush(Context.CommandList);
	}
}

CGpuScene::SModelEntry& CGpuScene::EnsureModel (CRenderContext& Context, const SRenderModel& Model, bool& bOutChanged)
{
	SModelEntry& entry = Models[&Model];
	entry.LastUsedFrame = FrameNumber;

	uint64 opacity = kHashSeed;
	for (uint32 s = 0; s < Model.Sections.Count(); ++s)
	{
		const uint32 mi = Model.Sections[s].MaterialIndex;
		const SMaterial* material = mi < Model.Materials.Count() ? Model.Materials[mi].GetRawIgnoringLifetime() : nullptr;
		opacity = HashValue(opacity, IsMaterialNonOpaque(material));
	}

	const bool bGeometryStale = entry.ModelIdentity != Model.Identity.Value || entry.GeometryVersion != Model.GeometryVersion || !entry.RtVertices.IsValid();
	if (!bGeometryStale && entry.OpacitySignature == opacity && entry.Blas.IsValid())
	{
		return entry;
	}
	bOutChanged = true;

	if (bGeometryStale)
	{
		FRT_GPU_SCOPE(Context.CommandList, "Scene_UploadModel");
		Device->Destroy(entry.RtVertices);
		Device->Destroy(entry.RasterVertices);
		Device->Destroy(entry.Indices);

		const uint32 vertexCount = Model.Vertices.Count();
		std::vector<uint8> rt(static_cast<size_t>(vertexCount) * gpu::RtVertexStride);
		DirectX::XMFLOAT3 bmin = { 1e30f, 1e30f, 1e30f };
		DirectX::XMFLOAT3 bmax = { -1e30f, -1e30f, -1e30f };
		for (uint32 i = 0; i < vertexCount; ++i)
		{
			const SVertex& v = Model.Vertices[i];
			uint32* dst = reinterpret_cast<uint32*>(rt.data() + static_cast<size_t>(i) * gpu::RtVertexStride);
			memcpy(dst, &v.Position, 12u);
			bmin = { (std::min)(bmin.x, v.Position.x), (std::min)(bmin.y, v.Position.y), (std::min)(bmin.z, v.Position.z) };
			bmax = { (std::max)(bmax.x, v.Position.x), (std::max)(bmax.y, v.Position.y), (std::max)(bmax.z, v.Position.z) };

			float nu, nv;
			PackOctahedral(v.Normal.x, v.Normal.y, v.Normal.z, nu, nv);
			dst[3] = PackSnorm2x16(nu, nv);

			// Tangent sign: does the stored bitangent agree with cross(N, T)?
			const float cx = v.Normal.y * v.Tangent.z - v.Normal.z * v.Tangent.y;
			const float cy = v.Normal.z * v.Tangent.x - v.Normal.x * v.Tangent.z;
			const float cz = v.Normal.x * v.Tangent.y - v.Normal.y * v.Tangent.x;
			const float handed = cx * v.Bitangent.x + cy * v.Bitangent.y + cz * v.Bitangent.z;
			float tu, tv;
			PackOctahedral(v.Tangent.x, v.Tangent.y, v.Tangent.z, tu, tv);
			dst[4] = (PackSnorm2x16(tu, tv) & ~1u) | (handed < 0.0f ? 1u : 0u);
			memcpy(dst + 5, &v.Uv, 8u);
		}
		entry.BoundsMin = vertexCount ? bmin : DirectX::XMFLOAT3{};
		entry.BoundsMax = vertexCount ? bmax : DirectX::XMFLOAT3{};

		const D3D12_RESOURCE_STATES readState =
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

		rhi::SBufferDesc rtDesc;
		rtDesc.Size = rt.size();
		rtDesc.SrvKind = rhi::EBufferViewKind::Raw;
		rtDesc.Name = "RtVertices";
		entry.RtVertices = Device->CreateBuffer(rtDesc);
		Device->UploadBuffer(Context.CommandList, entry.RtVertices, rt.data(), rt.size(), 0u, readState);

		rhi::SBufferDesc rasterDesc;
		rasterDesc.Size = static_cast<uint64>(vertexCount) * sizeof(SVertex);
		rasterDesc.Name = "RasterVertices";
		entry.RasterVertices = Device->CreateBuffer(rasterDesc);
		Device->UploadBuffer(Context.CommandList, entry.RasterVertices, Model.Vertices.GetData(), rasterDesc.Size, 0u,
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

		rhi::SBufferDesc ibDesc;
		ibDesc.Size = static_cast<uint64>(Model.Indices.Count()) * sizeof(uint32);
		ibDesc.SrvKind = rhi::EBufferViewKind::Raw;
		ibDesc.Name = "Indices";
		entry.Indices = Device->CreateBuffer(ibDesc);
		Device->UploadBuffer(Context.CommandList, entry.Indices, Model.Indices.GetData(), ibDesc.Size, 0u,
			readState | D3D12_RESOURCE_STATE_INDEX_BUFFER);

		entry.ModelIdentity = Model.Identity.Value;
		entry.GeometryVersion = Model.GeometryVersion;
		entry.SectionCount = Model.Sections.Count();
		entry.TriangleCount = Model.Indices.Count() / 3u;
		bGeometryTableDirty = true;
	}

	entry.OpacitySignature = opacity;
	BuildBlas(Context, Model, entry);
	return entry;
}

void CGpuScene::BuildBlas (CRenderContext& Context, const SRenderModel& Model, SModelEntry& Entry)
{
	FRT_GPU_SCOPE(Context.CommandList, "Scene_BuildBlas");
	ID3D12Device5* device = Device->GetDevice();

	std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
	geometries.reserve(Model.Sections.Count());
	for (uint32 s = 0; s < Model.Sections.Count(); ++s)
	{
		const SRenderSection& section = Model.Sections[s];
		const uint32 mi = section.MaterialIndex;
		const SMaterial* material = mi < Model.Materials.Count() ? Model.Materials[mi].GetRawIgnoringLifetime() : nullptr;

		D3D12_RAYTRACING_GEOMETRY_DESC g = {};
		g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		// Opaque geometry never invokes any-hit; only alpha-tested/blended sections pay for it.
		g.Flags = IsMaterialNonOpaque(material) ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		g.Triangles.IndexCount = section.IndexCount;
		g.Triangles.VertexCount = section.VertexCount;
		g.Triangles.IndexBuffer = Entry.Indices.GetGpuAddress() + static_cast<uint64>(section.IndexOffset) * sizeof(uint32);
		g.Triangles.VertexBuffer.StartAddress = Entry.RtVertices.GetGpuAddress() + static_cast<uint64>(section.VertexOffset) * gpu::RtVertexStride;
		g.Triangles.VertexBuffer.StrideInBytes = gpu::RtVertexStride;
		geometries.push_back(g);
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
	inputs.NumDescs = static_cast<UINT>(geometries.size());
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.pGeometryDescs = geometries.data();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
	device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

	if (!BlasScratch.IsValid() || BlasScratch.Size < prebuild.ScratchDataSizeInBytes)
	{
		Device->Destroy(BlasScratch);
		rhi::SBufferDesc scratch;
		scratch.Size = rhi::AlignUp((std::max<uint64>)(prebuild.ScratchDataSizeInBytes, 1ull << 20u), 65536u);
		scratch.UavKind = rhi::EBufferViewKind::None;
		scratch.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		scratch.Name = "BlasScratch";
		BlasScratch = Device->CreateBuffer(scratch);
	}

	Device->Destroy(Entry.Blas);
	rhi::SBufferDesc result;
	result.Size = rhi::AlignUp(prebuild.ResultDataMaxSizeInBytes, 256u);
	result.bAccelerationStructure = true;
	result.Name = "Blas";
	Entry.Blas = Device->CreateBuffer(result);
	Entry.bCompacted = false;
	Entry.bCompactionQueried = false;

	rhi::CBarrierBatch barriers;
	barriers.Transition(BlasScratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	barriers.Flush(Context.CommandList);

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
	build.Inputs = inputs;
	build.DestAccelerationStructureData = Entry.Blas.GetGpuAddress();
	build.ScratchAccelerationStructureData = BlasScratch.GetGpuAddress();

	// Ask for the compacted size in the same call; it is read back a couple of frames later.
	if (!Entry.CompactedSizeQuery.IsValid())
	{
		rhi::SBufferDesc query;
		query.Size = 16u;
		query.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		query.Name = "BlasCompactedSize";
		Entry.CompactedSizeQuery = Device->CreateBuffer(query);
		rhi::SBufferDesc readback;
		readback.Size = 16u;
		readback.HeapType = D3D12_HEAP_TYPE_READBACK;
		readback.Name = "BlasCompactedSizeReadback";
		Entry.CompactedSizeReadback = Device->CreateBuffer(readback);
	}
	barriers.Transition(Entry.CompactedSizeQuery, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	barriers.Flush(Context.CommandList);

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postbuild = {};
	postbuild.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
	postbuild.DestBuffer = Entry.CompactedSizeQuery.GetGpuAddress();
	Context.CommandList->BuildRaytracingAccelerationStructure(&build, 1, &postbuild);

	// Scratch is reused by the next build; the BLAS is read by the TLAS build.
	barriers.Uav(BlasScratch);
	barriers.Uav(Entry.Blas);
	barriers.Transition(Entry.CompactedSizeQuery, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barriers.Flush(Context.CommandList);
	Context.CommandList->CopyBufferRegion(Entry.CompactedSizeReadback.Resource.Get(), 0, Entry.CompactedSizeQuery.Resource.Get(), 0, 8u);
	Entry.CompactionRequestFrame = FrameNumber;
	Entry.bCompactionQueried = true;
}

void CGpuScene::UpdateCompaction (CRenderContext& Context)
{
	for (auto& [model, entry] : Models)
	{
		if (entry.bCompacted || !entry.bCompactionQueried || !entry.Blas.IsValid())
		{
			continue;
		}
		// The readback was recorded CompactionRequestFrame; with MaxFramesInFlight frames in
		// flight it is guaranteed complete once we are that many frames past it.
		if (FrameNumber < entry.CompactionRequestFrame + rhi::MaxFramesInFlight + 1u)
		{
			continue;
		}
		uint64 compactedSize = 0u;
		memcpy(&compactedSize, entry.CompactedSizeReadback.Mapped, sizeof(uint64));
		entry.bCompactionQueried = false;
		if (compactedSize == 0u || compactedSize >= entry.Blas.Size)
		{
			entry.bCompacted = true;
			continue;
		}

		FRT_GPU_SCOPE(Context.CommandList, "Scene_CompactBlas");
		rhi::SBufferDesc desc;
		desc.Size = rhi::AlignUp(compactedSize, 256u);
		desc.bAccelerationStructure = true;
		desc.Name = "BlasCompacted";
		rhi::SGpuBuffer compacted = Device->CreateBuffer(desc);
		Context.CommandList->CopyRaytracingAccelerationStructure(
			compacted.GetGpuAddress(), entry.Blas.GetGpuAddress(),
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
		rhi::CBarrierBatch barriers;
		barriers.Uav(compacted);
		barriers.Flush(Context.CommandList);
		Device->Destroy(entry.Blas);
		entry.Blas = std::move(compacted);
		entry.bCompacted = true;
	}
}

void CGpuScene::WriteMaterialData (const SMaterial& Material, SMaterialEntry& Entry)
{
	gpu::SMaterialData& data = MaterialTable[Entry.Index];
	data.BaseColorFactor = { Material.DiffuseAlbedo.R, Material.DiffuseAlbedo.G, Material.DiffuseAlbedo.B, Material.DiffuseAlbedo.A };
	const float e = Material.IsEmissive() ? Material.EmissiveIntensity : 0.0f;
	data.EmissiveFactor = { Material.Emissive.R * e, Material.Emissive.G * e, Material.Emissive.B * e };
	data.AlphaCutoff = Material.AlphaCutoff;
	data.Metallic = Material.Metallic;
	data.Roughness = Material.Roughness;
	data.NormalScale = Material.NormalScale;
	data.SpecularFactor = Material.SpecularFactor;
	data.BaseColorTexture = Textures.GetSrv(Entry.BaseColor);
	data.NormalTexture = Textures.GetSrv(Entry.Normal);
	data.MetallicRoughnessTexture = Textures.GetSrv(Entry.MetallicRoughness);
	data.EmissiveTexture = Textures.GetSrv(Entry.Emissive);
	data.Flags = 0u;
	if (Material.AlphaMode == EAlphaMode::Mask) { data.Flags |= gpu::MaterialFlag_AlphaMask; }
	if (Material.AlphaMode == EAlphaMode::Blend) { data.Flags |= gpu::MaterialFlag_AlphaBlend; }
	if (Material.bDoubleSided) { data.Flags |= gpu::MaterialFlag_DoubleSided; }
	data.Ior = Material.Ior;
	data.Transmission = Material.Transmission;
	data.Pad0 = 0.0f;
}

uint32 CGpuScene::EnsureMaterial (const SMaterial* Material, bool& bOutChanged)
{
	if (!Material)
	{
		return 0u; // table slot 0 is the default material
	}
	auto it = MaterialEntries.find(Material);
	if (it == MaterialEntries.end())
	{
		SMaterialEntry entry;
		entry.Index = static_cast<uint32>(MaterialTable.size());
		MaterialTable.push_back({});
		it = MaterialEntries.emplace(Material, entry).first;
	}
	SMaterialEntry& entry = it->second;
	if (entry.Version != Material->Version || !entry.bTexturesResolved)
	{
		entry.BaseColor = Textures.Request(Material->ResolveTexturePath(Material->BaseColorTexturePath), ETextureColorSpace::Srgb);
		entry.Normal = Textures.Request(Material->ResolveTexturePath(Material->NormalTexturePath), ETextureColorSpace::Normal);
		entry.MetallicRoughness = Textures.Request(Material->ResolveTexturePath(Material->MetallicRoughnessTexturePath), ETextureColorSpace::Linear);
		entry.Emissive = Textures.Request(Material->ResolveTexturePath(Material->EmissiveTexturePath), ETextureColorSpace::Srgb);
		entry.bTexturesResolved = true;
		entry.Version = Material->Version;
		bMaterialTableDirty = true;
		bOutChanged = true;
	}
	if (bMaterialTableDirty)
	{
		WriteMaterialData(*Material, entry);
	}
	return entry.Index;
}

bool CGpuScene::Update (CRenderContext& Context)
{
	++FrameNumber;
	bool bChanged = false;
	ID3D12GraphicsCommandList4* cmd = Context.CommandList;

	if (MaterialTable.empty())
	{
		// Slot 0: neutral grey dielectric for meshes without a material.
		gpu::SMaterialData def = {};
		def.BaseColorFactor = { 0.8f, 0.8f, 0.8f, 1.0f };
		def.Roughness = 0.6f;
		def.NormalScale = 1.0f;
		def.SpecularFactor = 1.0f;
		def.AlphaCutoff = 0.5f;
		def.Ior = 1.5f;
		def.BaseColorTexture = def.NormalTexture = def.MetallicRoughnessTexture = def.EmissiveTexture = FRT_INVALID_INDEX;
		MaterialTable.push_back(def);
	}

	if (!bEnergyLutBaked)
	{
		BakeEnergyLut(Context);
	}

	{
		FRT_GPU_SCOPE(cmd, "Scene_TextureUploads");
		if (Textures.Update(cmd))
		{
			// New texture residency changes material entries (and the image).
			bMaterialTableDirty = true;
			bChanged = true;
		}
	}

	// Models (lazy upload + BLAS), compaction of earlier builds.
	for (const SSceneInstance& instance : FrameInstances)
	{
		EnsureModel(Context, *instance.Model, bChanged);
	}
	UpdateCompaction(Context);

	// Evict models nobody has drawn for a while (e.g. destroyed entities).
	for (auto it = Models.begin(); it != Models.end();)
	{
		if (FrameNumber > it->second.LastUsedFrame + 600u)
		{
			Device->Destroy(it->second.RtVertices);
			Device->Destroy(it->second.RasterVertices);
			Device->Destroy(it->second.Indices);
			Device->Destroy(it->second.Blas);
			Device->Destroy(it->second.CompactedSizeQuery);
			Device->Destroy(it->second.CompactedSizeReadback);
			it = Models.erase(it);
			bGeometryTableDirty = true;
		}
		else
		{
			++it;
		}
	}

	// Materials of every visible section.
	{
		// First pass may grow the table; a second pass writes entries if anything got dirty.
		for (uint32 pass = 0; pass < 2; ++pass)
		{
			for (const SSceneInstance& instance : FrameInstances)
			{
				const SRenderModel& model = *instance.Model;
				for (uint32 m = 0; m < model.Materials.Count(); ++m)
				{
					EnsureMaterial(model.Materials[m].GetRawIgnoringLifetime(), bChanged);
				}
			}
			if (!bMaterialTableDirty)
			{
				break;
			}
		}
	}

	// Geometry table: one contiguous range per model, ordered deterministically by first use.
	uint64 layoutHash = kHashSeed;
	std::vector<const SRenderModel*> modelOrder;
	{
		std::unordered_map<const SRenderModel*, bool> seen;
		for (const SSceneInstance& instance : FrameInstances)
		{
			if (seen.emplace(instance.Model, true).second)
			{
				modelOrder.push_back(instance.Model);
				layoutHash = HashValue(layoutHash, instance.Model);
				layoutHash = HashValue(layoutHash, Models[instance.Model].GeometryVersion);
			}
		}
	}
	if (layoutHash != GeometryLayoutHash)
	{
		bGeometryTableDirty = true;
		GeometryLayoutHash = layoutHash;
	}
	if (bGeometryTableDirty || bMaterialTableDirty)
	{
		GeometryTable.clear();
		for (const SRenderModel* model : modelOrder)
		{
			SModelEntry& entry = Models[model];
			entry.GeometryBase = static_cast<uint32>(GeometryTable.size());
			uint32 emissiveLocal = 0u;
			for (uint32 s = 0; s < model->Sections.Count(); ++s)
			{
				const SRenderSection& section = model->Sections[s];
				const SMaterial* material = section.MaterialIndex < model->Materials.Count()
												? model->Materials[section.MaterialIndex].GetRawIgnoringLifetime()
												: nullptr;
				bool unused = false;
				gpu::SGeometryData g = {};
				g.VertexBufferSrv = entry.RtVertices.Srv;
				g.IndexBufferSrv = entry.Indices.Srv;
				g.VertexOffset = section.VertexOffset;
				g.IndexOffset = section.IndexOffset;
				g.MaterialIndex = EnsureMaterial(material, unused);
				g.TriangleCount = section.IndexCount / 3u;
				g.Flags = 0u;
				if (material && material->AlphaMode == EAlphaMode::Mask) { g.Flags |= gpu::GeometryFlag_AlphaTested; }
				if (material && material->AlphaMode == EAlphaMode::Blend) { g.Flags |= gpu::GeometryFlag_AlphaBlend; }
				if (material && material->bDoubleSided) { g.Flags |= gpu::GeometryFlag_DoubleSided; }
				g.EmissiveLocalBase = FRT_INVALID_INDEX;
				if (material && material->IsEmissive())
				{
					g.Flags |= gpu::GeometryFlag_Emissive;
					g.EmissiveLocalBase = emissiveLocal;
					emissiveLocal += g.TriangleCount;
				}
				GeometryTable.push_back(g);
			}
			entry.EmissiveTriangleCount = emissiveLocal;
		}
		UploadTable(Context, GeometryBuffer, GeometryTable.data(), GeometryTable.size() * sizeof(gpu::SGeometryData), sizeof(gpu::SGeometryData), "GeometryTable");
		bGeometryTableDirty = false;
		bChanged = true;
	}
	if (bMaterialTableDirty)
	{
		UploadTable(Context, MaterialBuffer, MaterialTable.data(), MaterialTable.size() * sizeof(gpu::SMaterialData), sizeof(gpu::SMaterialData), "MaterialTable");
		bMaterialTableDirty = false;
		bChanged = true;
	}

	// Instances + emissive triangle ranges.
	InstanceTable.resize(FrameInstances.size());
	uint64 instanceHash = kHashSeed;
	uint64 emissiveHash = kHashSeed;
	uint32 emissiveCursor = 0u;
	DirectX::XMFLOAT3 sceneMin = { 1e30f, 1e30f, 1e30f };
	DirectX::XMFLOAT3 sceneMax = { -1e30f, -1e30f, -1e30f };
	std::unordered_map<uint64, DirectX::XMFLOAT3X4> currentTransforms;
	currentTransforms.reserve(FrameInstances.size());
	for (size_t i = 0; i < FrameInstances.size(); ++i)
	{
		const SSceneInstance& src = FrameInstances[i];
		const SModelEntry& model = Models[src.Model];
		gpu::SInstanceData& dst = InstanceTable[i];
		StoreRows(src.ObjectToWorld, dst.ObjectToWorld);
		const auto prev = PreviousTransforms.find(src.Key);
		StoreRows(prev != PreviousTransforms.end() ? prev->second : src.ObjectToWorld, dst.PrevObjectToWorld);
		const bool bNegative = ComputeNormalMatrix(src.ObjectToWorld, dst.NormalToWorld);
		dst.GeometryBase = model.GeometryBase;
		dst.GeometryCount = model.SectionCount;
		dst.Flags = (bNegative ? gpu::InstanceFlag_NegativeDeterminant : 0u) | (src.bHelper ? gpu::InstanceFlag_Helper : 0u);
		dst.EmissiveTriangleBase = FRT_INVALID_INDEX;
		if (model.EmissiveTriangleCount > 0u && !src.bHelper)
		{
			dst.EmissiveTriangleBase = emissiveCursor;
			emissiveCursor += model.EmissiveTriangleCount;
			emissiveHash = HashValue(emissiveHash, src.Model);
			emissiveHash = HashValue(emissiveHash, i);
		}
		currentTransforms[src.Key] = src.ObjectToWorld;
		instanceHash = HashValue(instanceHash, src.Model);
		instanceHash = HashValue(instanceHash, src.ObjectToWorld);
		instanceHash = HashValue(instanceHash, src.bHelper);

		if (!src.bHelper)
		{
			for (uint32 c = 0; c < 8u; ++c)
			{
				const DirectX::XMFLOAT3 corner = TransformPoint(src.ObjectToWorld,
					(c & 1u) ? model.BoundsMax.x : model.BoundsMin.x,
					(c & 2u) ? model.BoundsMax.y : model.BoundsMin.y,
					(c & 4u) ? model.BoundsMax.z : model.BoundsMin.z);
				sceneMin = { (std::min)(sceneMin.x, corner.x), (std::min)(sceneMin.y, corner.y), (std::min)(sceneMin.z, corner.z) };
				sceneMax = { (std::max)(sceneMax.x, corner.x), (std::max)(sceneMax.y, corner.y), (std::max)(sceneMax.z, corner.z) };
			}
		}
	}
	PreviousTransforms = std::move(currentTransforms);
	if (!FrameInstances.empty() && sceneMax.x >= sceneMin.x)
	{
		SceneCenter = { 0.5f * (sceneMin.x + sceneMax.x), 0.5f * (sceneMin.y + sceneMax.y), 0.5f * (sceneMin.z + sceneMax.z) };
		const float dx = sceneMax.x - sceneMin.x, dy = sceneMax.y - sceneMin.y, dz = sceneMax.z - sceneMin.z;
		SceneRadius = (std::max)(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), 1.0f);
	}
	if (instanceHash != InstanceHash)
	{
		InstanceHash = instanceHash;
		bChanged = true;
	}
	UploadTable(Context, InstanceBuffer, InstanceTable.data(), InstanceTable.size() * sizeof(gpu::SInstanceData), sizeof(gpu::SInstanceData), "InstanceTable");

	// Emissive triangles (rebuilt only when the set of emissive instances changes).
	emissiveHash = HashValue(emissiveHash, GeometryLayoutHash);
	// Streamed emissive textures change the per-triangle power estimates.
	emissiveHash = HashValue(emissiveHash, Textures.GetResidentCount());
	if (emissiveHash != EmissiveLayoutHash)
	{
		EmissiveLayoutHash = emissiveHash;
		EmissiveTriangles.clear();
		EmissiveTriangles.reserve(emissiveCursor);
		for (size_t i = 0; i < FrameInstances.size(); ++i)
		{
			const gpu::SInstanceData& inst = InstanceTable[i];
			if (inst.EmissiveTriangleBase == FRT_INVALID_INDEX)
			{
				continue;
			}
			const SRenderModel& model = *FrameInstances[i].Model;
			const DirectX::XMFLOAT3X4& xf = FrameInstances[i].ObjectToWorld;
			for (uint32 s = 0; s < model.Sections.Count(); ++s)
			{
				const gpu::SGeometryData& g = GeometryTable[inst.GeometryBase + s];
				if (g.EmissiveLocalBase == FRT_INVALID_INDEX)
				{
					continue;
				}
				const gpu::SMaterialData& m = MaterialTable[g.MaterialIndex];
				const SRenderSection& section = model.Sections[s];
				const SMaterial* material = model.Materials[section.MaterialIndex].GetRawIgnoringLifetime();
				DirectX::XMFLOAT4 avg = { 1.0f, 1.0f, 1.0f, 1.0f };
				if (material)
				{
					const auto entryIt = MaterialEntries.find(material);
					if (entryIt != MaterialEntries.end())
					{
						avg = Textures.GetAverageColor(entryIt->second.Emissive);
					}
				}
				const float er = m.EmissiveFactor.x * avg.x;
				const float eg = m.EmissiveFactor.y * avg.y;
				const float eb = m.EmissiveFactor.z * avg.z;
				for (uint32 t = 0; t < g.TriangleCount; ++t)
				{
					const uint32 i0 = model.Indices[section.IndexOffset + t * 3u + 0u] + section.VertexOffset;
					const uint32 i1 = model.Indices[section.IndexOffset + t * 3u + 1u] + section.VertexOffset;
					const uint32 i2 = model.Indices[section.IndexOffset + t * 3u + 2u] + section.VertexOffset;
					const Vector3f& a = model.Vertices[i0].Position;
					const Vector3f& b = model.Vertices[i1].Position;
					const Vector3f& c = model.Vertices[i2].Position;
					const DirectX::XMFLOAT3 pa = TransformPoint(xf, a.x, a.y, a.z);
					const DirectX::XMFLOAT3 pb = TransformPoint(xf, b.x, b.y, b.z);
					const DirectX::XMFLOAT3 pc = TransformPoint(xf, c.x, c.y, c.z);
					const float e1x = pb.x - pa.x, e1y = pb.y - pa.y, e1z = pb.z - pa.z;
					const float e2x = pc.x - pa.x, e2y = pc.y - pa.y, e2z = pc.z - pa.z;
					const float cx = e1y * e2z - e1z * e2y, cy = e1z * e2x - e1x * e2z, cz = e1x * e2y - e1y * e2x;
					const float area = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);

					gpu::SEmissiveTriangle tri = {};
					tri.InstanceIndex = static_cast<uint32>(i);
					tri.GeometryIndex = inst.GeometryBase + s;
					tri.PrimitiveIndex = t;
					tri.AverageRadiance = { er, eg, eb };
					tri.Area = area;
					// Two-sided emitters radiate pi * L per unit area per side.
					tri.Power = Luminance(er, eg, eb) * area * kPi;
					EmissiveTriangles.push_back(tri);
				}
			}
		}
		UploadTable(Context, EmissiveBuffer, EmissiveTriangles.data(), EmissiveTriangles.size() * sizeof(gpu::SEmissiveTriangle), sizeof(gpu::SEmissiveTriangle), "EmissiveTriangles");
		bChanged = true;
		LightHash = 0u; // force the alias table to be rebuilt
	}
	else
	{
		// Instance order may have shifted without changing the emissive set's identity hash
		// (hash includes indices, so this branch only keeps the buffer valid).
		if (!EmissiveBuffer.IsValid())
		{
			UploadTable(Context, EmissiveBuffer, nullptr, 0u, sizeof(gpu::SEmissiveTriangle), "EmissiveTriangles");
		}
	}

	// Sky bake (only when its parameters change).
	{
		uint64 skyHash = kHashSeed;
		skyHash = HashValue(skyHash, FrameSky.Constants);
		skyHash = HashValue(skyHash, FrameSky.bSkyEnabled);
		if (skyHash != SkyHash)
		{
			SkyHash = skyHash;
			BakeSky(Context);
			bChanged = true;
		}
	}

	BuildLightTables(Context);

	// Portals.
	UploadTable(Context, PortalBuffer, FramePortals.data(), FramePortals.size() * sizeof(gpu::SPortalData), sizeof(gpu::SPortalData), "Portals");

	BuildTlas(Context);

	// Raster preview draws.
	Draws.clear();
	for (size_t i = 0; i < FrameInstances.size(); ++i)
	{
		const SRenderModel& model = *FrameInstances[i].Model;
		const SModelEntry& entry = Models[&model];
		for (uint32 s = 0; s < model.Sections.Count(); ++s)
		{
			const SRenderSection& section = model.Sections[s];
			SSceneDraw draw;
			draw.VertexBuffer.BufferLocation = entry.RasterVertices.GetGpuAddress();
			draw.VertexBuffer.SizeInBytes = static_cast<UINT>(entry.RasterVertices.Size);
			draw.VertexBuffer.StrideInBytes = sizeof(SVertex);
			draw.IndexBuffer.BufferLocation = entry.Indices.GetGpuAddress();
			draw.IndexBuffer.SizeInBytes = static_cast<UINT>(entry.Indices.Size);
			draw.IndexBuffer.Format = DXGI_FORMAT_R32_UINT;
			draw.InstanceIndex = static_cast<uint32>(i);
			draw.GeometryIndex = entry.GeometryBase + s;
			draw.IndexCount = section.IndexCount;
			draw.IndexOffset = section.IndexOffset;
			draw.VertexOffset = static_cast<int32>(section.VertexOffset);
			const SMaterial* material = section.MaterialIndex < model.Materials.Count()
											? model.Materials[section.MaterialIndex].GetRawIgnoringLifetime() : nullptr;
			draw.bAlphaBlend = material && material->AlphaMode == EAlphaMode::Blend;
			Draws.push_back(draw);
		}
	}

	// Stats.
	Stats = {};
	Stats.Instances = static_cast<uint32>(FrameInstances.size());
	Stats.Models = static_cast<uint32>(Models.size());
	for (const auto& [model, entry] : Models)
	{
		Stats.BlasBytes += entry.Blas.Size;
		Stats.BlasCompacted += entry.bCompacted ? 1u : 0u;
	}
	for (const SSceneInstance& instance : FrameInstances)
	{
		Stats.Triangles += instance.Model->Indices.Count() / 3u;
	}
	Stats.Materials = static_cast<uint32>(MaterialTable.size());
	Stats.AnalyticLights = static_cast<uint32>(LightTable.size());
	Stats.EmissiveTriangles = static_cast<uint32>(EmissiveTriangles.size());
	Stats.TexturesResident = Textures.GetResidentCount();
	Stats.TexturesPending = Textures.GetPendingCount();
	Stats.TextureBytes = Textures.GetResidentBytes();

	return bChanged;
}

void CGpuScene::BuildLightTables (CRenderContext& Context)
{
	LightTable.clear();
	SunLightIndex = FRT_INVALID_INDEX;
	SkyLightIndex = FRT_INVALID_INDEX;

	const SSkyConstants& sky = FrameSky.Constants;
	if (FrameSky.bSunEnabled && sky.SunIntensity > 0.0f)
	{
		gpu::SLightData sun = {};
		sun.Type = gpu::LightType_Sun;
		sun.Flags = gpu::LightFlag_CastsShadows;
		const float cosRadius = std::cos((std::max)(FrameSky.SunAngularRadius, 1e-4f));
		sun.Radius = cosRadius;
		sun.SolidAngle = 2.0f * kPi * (1.0f - cosRadius);
		// SunDirection points from the sun toward the scene; lights store the direction TO it.
		float dx = -sky.SunDirection.x, dy = -sky.SunDirection.y, dz = -sky.SunDirection.z;
		const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (len > 0.0f) { dx /= len; dy /= len; dz /= len; }
		sun.AxisU = { dx, dy, dz };
		// SunIntensity is the irradiance at normal incidence; spread it over the disk.
		const float scale = sky.SunIntensity / sun.SolidAngle;
		sun.Radiance = { sky.SunColor.R * scale, sky.SunColor.G * scale, sky.SunColor.B * scale };
		SunLightIndex = static_cast<uint32>(LightTable.size());
		LightTable.push_back(sun);
	}
	if (FrameSky.bSkyEnabled && sky.SkyIntensity > 0.0f)
	{
		gpu::SLightData skyLight = {};
		skyLight.Type = gpu::LightType_Sky;
		SkyLightIndex = static_cast<uint32>(LightTable.size());
		LightTable.push_back(skyLight);
	}
	for (const gpu::SLightData& light : FrameLights)
	{
		LightTable.push_back(light);
	}

	// Selection weights: estimated emitted power (pbrt PowerLightSampler-style). Distant lights
	// are weighted by the power they deliver to a disk of the scene radius; RIS corrects the
	// global estimate per shading point. With portals the scene is a closed interior that sun
	// and sky only enter through the portal openings: weight them by the power crossing those
	// (projected) openings instead. Over the scene disk they would take nearly every NEE sample
	// even where they are occluded, starving the interior emitters (whose light then arrives by
	// BSDF sampling alone, as fireflies). Selection only changes noise, never the expectation.
	std::vector<float> weights;
	weights.reserve(LightTable.size() + EmissiveTriangles.size());
	const float sceneArea = kPi * SceneRadius * SceneRadius;
	float skyArea = sceneArea;
	float portalArea = 0.0f;
	if (bPortalWeightedLightSelection)
	{
		for (const gpu::SPortalData& portal : FramePortals)
		{
			portalArea += portal.Area;
		}
	}
	if (portalArea > 0.0f)
	{
		skyArea = portalArea;
	}
	float sunPower = 0.0f, skyPower = 0.0f, analyticPower = 0.0f, emissivePower = 0.0f;
	for (const gpu::SLightData& light : LightTable)
	{
		float power = 0.0f;
		switch (light.Type)
		{
			case gpu::LightType_Sun:
			{
				float sunArea = sceneArea;
				if (portalArea > 0.0f)
				{
					sunArea = 0.0f;
					for (const gpu::SPortalData& portal : FramePortals)
					{
						const float c = portal.Normal.x * light.AxisU.x + portal.Normal.y * light.AxisU.y + portal.Normal.z * light.AxisU.z;
						sunArea += portal.Area * std::abs(c);
					}
				}
				power = Luminance(light.Radiance.x, light.Radiance.y, light.Radiance.z) * light.SolidAngle * sunArea;
				sunPower += power;
				break;
			}
			case gpu::LightType_Sky:
				power = SkyAverageLuminance * kPi * skyArea;
				skyPower += power;
				break;
			case gpu::LightType_Sphere:
			case gpu::LightType_Spot:
				power = light.Radius > 0.0f
							? Luminance(light.Radiance.x, light.Radiance.y, light.Radiance.z) * 4.0f * kPi * kPi * light.Radius * light.Radius
							: Luminance(light.Radiance.x, light.Radiance.y, light.Radiance.z) * 4.0f * kPi;
				break;
			case gpu::LightType_Rect:
			{
				const float u = std::sqrt(light.AxisU.x * light.AxisU.x + light.AxisU.y * light.AxisU.y + light.AxisU.z * light.AxisU.z);
				const float v = std::sqrt(light.AxisV.x * light.AxisV.x + light.AxisV.y * light.AxisV.y + light.AxisV.z * light.AxisV.z);
				const float sides = (light.Flags & gpu::LightFlag_TwoSided) ? 2.0f : 1.0f;
				power = Luminance(light.Radiance.x, light.Radiance.y, light.Radiance.z) * kPi * 4.0f * u * v * sides;
				break;
			}
			default:
				break;
		}
		if (light.Type != gpu::LightType_Sun && light.Type != gpu::LightType_Sky)
		{
			analyticPower += (std::max)(power, 0.0f);
		}
		weights.push_back((std::max)(power, 0.0f));
	}
	for (gpu::SLightData& light : LightTable)
	{
		light.SelectionPower = weights[&light - LightTable.data()];
	}
	for (const gpu::SEmissiveTriangle& tri : EmissiveTriangles)
	{
		weights.push_back(tri.Power);
		emissivePower += tri.Power;
	}

	uint64 hash = kHashSeed;
	hash = HashBytes(hash, LightTable.data(), LightTable.size() * sizeof(gpu::SLightData));
	hash = HashValue(hash, EmissiveTriangles.size());
	if (hash != LightHash)
	{
		LightHash = hash;
		const float total = (std::max)(sunPower + skyPower + analyticPower + emissivePower, 1e-30f);
		std::fprintf(stdout, "[lights] selection: sun %.3f sky %.3f analytic %.3f emissive %.3f (%zu triangles)%s\n",
			sunPower / total, skyPower / total, analyticPower / total, emissivePower / total, EmissiveTriangles.size(),
			portalArea > 0.0f ? " [portal-weighted distant lights]" : "");
		std::fflush(stdout);
		BuildAliasTable(weights, AliasTable);
		UploadTable(Context, AliasBuffer, AliasTable.data(), AliasTable.size() * sizeof(gpu::SAliasEntry), sizeof(gpu::SAliasEntry), "LightAliasTable");
	}
	UploadTable(Context, LightBuffer, LightTable.data(), LightTable.size() * sizeof(gpu::SLightData), sizeof(gpu::SLightData), "Lights");
}

void CGpuScene::BuildTlas (CRenderContext& Context)
{
	FRT_GPU_SCOPE(Context.CommandList, "Scene_BuildTlas");
	ID3D12Device5* device = Device->GetDevice();

	const uint32 count = static_cast<uint32>(FrameInstances.size());
	rhi::SUploadAllocation descs = Device->GetUploadRing().Allocate(
		(std::max<uint64>)(count, 1u) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), 16u);
	D3D12_RAYTRACING_INSTANCE_DESC* out = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(descs.Cpu);
	for (uint32 i = 0; i < count; ++i)
	{
		const SSceneInstance& src = FrameInstances[i];
		D3D12_RAYTRACING_INSTANCE_DESC& d = out[i];
		memset(&d, 0, sizeof(d));
		memcpy(d.Transform, &src.ObjectToWorld, sizeof(d.Transform));
		d.InstanceID = 0u;
		d.InstanceMask = src.bHelper ? gpu::InstanceMask_Helper : gpu::InstanceMask_Scene;
		d.InstanceContributionToHitGroupIndex = 0u;
		// The path tracer decides facing itself; never let the hardware cull.
		d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
		d.AccelerationStructure = Models[src.Model].Blas.GetGpuAddress();
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	inputs.NumDescs = count;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.InstanceDescs = descs.Gpu;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
	device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

	CurrentTlas = (CurrentTlas + 1u) % rhi::MaxFramesInFlight;
	rhi::SGpuBuffer& tlas = Tlas[CurrentTlas];
	if (!tlas.IsValid() || tlas.Size < prebuild.ResultDataMaxSizeInBytes)
	{
		Device->Destroy(tlas);
		rhi::SBufferDesc desc;
		desc.Size = rhi::AlignUp((std::max<uint64>)(prebuild.ResultDataMaxSizeInBytes * 2u, 65536u), 256u);
		desc.bAccelerationStructure = true;
		desc.Name = "Tlas";
		tlas = Device->CreateBuffer(desc);
	}
	if (!TlasScratch.IsValid() || TlasScratch.Size < prebuild.ScratchDataSizeInBytes)
	{
		Device->Destroy(TlasScratch);
		rhi::SBufferDesc desc;
		desc.Size = rhi::AlignUp((std::max<uint64>)(prebuild.ScratchDataSizeInBytes * 2u, 65536u), 256u);
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		desc.Name = "TlasScratch";
		TlasScratch = Device->CreateBuffer(desc);
	}

	rhi::CBarrierBatch barriers;
	barriers.Transition(TlasScratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	barriers.Flush(Context.CommandList);

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
	build.Inputs = inputs;
	build.DestAccelerationStructureData = tlas.GetGpuAddress();
	build.ScratchAccelerationStructureData = TlasScratch.GetGpuAddress();
	Context.CommandList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

	barriers.Uav(tlas);
	barriers.Uav(TlasScratch);
	barriers.Flush(Context.CommandList);
}

void CGpuScene::BakeSky (CRenderContext& Context)
{
	FRT_GPU_SCOPE(Context.CommandList, "Scene_BakeSky");
	const SSkyConstants& sky = FrameSky.Constants;

	// CPU estimate of the mean sky luminance for the light selection weight (upper + lower
	// hemisphere average over solid angle, weighting rows by their extent in cos(theta)).
	{
		double sum = 0.0;
		const int rows = 64;
		for (int r = 0; r < rows; ++r)
		{
			const float y = -1.0f + (2.0f * r + 1.0f) / rows; // uniform in cos = uniform in solid angle
			const DirectX::XMFLOAT3 c = EvalProceduralSkyCpu(sky, y);
			sum += Luminance(c.x, c.y, c.z);
		}
		SkyAverageLuminance = FrameSky.bSkyEnabled ? static_cast<float>(sum / rows) : 0.0f;
	}

	Context.BindCompute();
	gpu::SSkyBakeConstants constants = {};
	constants.Zenith = { sky.SkyZenithColor.R, sky.SkyZenithColor.G, sky.SkyZenithColor.B, 0.0f };
	constants.Horizon = { sky.SkyHorizonColor.R, sky.SkyHorizonColor.G, sky.SkyHorizonColor.B, 0.0f };
	constants.Ground = { sky.GroundColor.R, sky.GroundColor.G, sky.GroundColor.B, 0.0f };
	constants.Intensity = sky.SkyIntensity;
	constants.HorizonSoftness = sky.HorizonSoftness;
	constants.Size = SkyEnvironmentSize;
	constants.OutEnvironment = SkyEnvironment.Uav;
	constants.OutLuminance = SkyPyramid.MipUavs[0];
	constants.InLuminance = FRT_INVALID_INDEX;
	constants.LuminanceFloor = (std::max)(SkyAverageLuminance * 1e-3f, 1e-6f);

	Context.Barriers.Transition(SkyEnvironment, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Context.Barriers.Transition(SkyPyramid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Context.PushPassConstants(constants);
	Context.Dispatch2D(SkyBakePipeline, SkyEnvironmentSize, SkyEnvironmentSize);

	uint32 size = SkyEnvironmentSize;
	for (uint32 mip = 1; mip < SkyPyramid.MipLevels; ++mip)
	{
		size = (std::max)(1u, size / 2u);
		gpu::SSkyBakeConstants down = constants;
		down.Size = size;
		down.InLuminance = SkyPyramid.MipUavs[mip - 1];
		down.OutLuminance = SkyPyramid.MipUavs[mip];
		Context.Barriers.Uav(SkyPyramid);
		Context.PushPassConstants(down);
		Context.Dispatch2D(SkyPyramidPipeline, size, size);
	}
	Context.Barriers.Uav(SkyPyramid);
	Context.Barriers.Transition(SkyEnvironment, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Context.Barriers.Transition(SkyPyramid, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Context.FlushBarriers();
}

void CGpuScene::BakeEnergyLut (CRenderContext& Context)
{
	if (!Pipelines->Get(EnergyLutPipeline))
	{
		return;
	}
	FRT_GPU_SCOPE(Context.CommandList, "Scene_BakeEnergyLut");
	Context.BindCompute();
	const uint32 constants[4] = { EnergyLut.Uav, 32u, 0u, 0u };
	Context.Barriers.Transition(EnergyLut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Context.FlushBarriers();
	Context.SetRootConstants(constants, 4u);
	Context.Dispatch2D(EnergyLutPipeline, 32u, 32u);
	Context.Barriers.Transition(EnergyLut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Context.FlushBarriers();
	bEnergyLutBaked = true;
}

void CGpuScene::FillFrameConstants (gpu::SFrameConstants& Frame) const
{
	Frame.InstanceBuffer = InstanceBuffer.Srv;
	Frame.GeometryBuffer = GeometryBuffer.Srv;
	Frame.MaterialBuffer = MaterialBuffer.Srv;
	Frame.LightBuffer = LightBuffer.Srv;
	Frame.LightCount = static_cast<uint32>(LightTable.size());
	Frame.EmissiveTriangleBuffer = EmissiveBuffer.Srv;
	Frame.EmissiveTriangleCount = static_cast<uint32>(EmissiveTriangles.size());
	Frame.LightAliasTable = AliasBuffer.Srv;
	Frame.TotalLightCount = Frame.LightCount + Frame.EmissiveTriangleCount;
	Frame.SkyEnvironmentMap = SkyEnvironment.Srv;
	Frame.SkyLuminancePyramid = SkyPyramid.Srv;
	Frame.SkyEnvironmentSize = SkyEnvironmentSize;
	Frame.SkyPyramidMips = SkyPyramid.MipLevels;
	Frame.PortalBuffer = PortalBuffer.Srv;
	Frame.PortalCount = static_cast<uint32>(FramePortals.size());
	Frame.EnergyCompensationLut = bEnergyLutBaked ? EnergyLut.Srv : FRT_INVALID_INDEX;
	Frame.SunLightIndex = SunLightIndex;
	Frame.SkyLightIndex = SkyLightIndex;
	Frame.SceneRadius = SceneRadius;

	const SSkyConstants& sky = FrameSky.Constants;
	Frame.SkyZenithColor = { sky.SkyZenithColor.R, sky.SkyZenithColor.G, sky.SkyZenithColor.B, 1.0f };
	Frame.SkyHorizonColor = { sky.SkyHorizonColor.R, sky.SkyHorizonColor.G, sky.SkyHorizonColor.B, 1.0f };
	Frame.GroundColor = { sky.GroundColor.R, sky.GroundColor.G, sky.GroundColor.B, 1.0f };
	Frame.SkyIntensity = sky.SkyIntensity;
	Frame.HorizonSoftness = sky.HorizonSoftness;
	if (SunLightIndex != FRT_INVALID_INDEX)
	{
		const gpu::SLightData& sun = LightTable[SunLightIndex];
		Frame.SunDirection = sun.AxisU;
		Frame.SunCosAngularRadius = sun.Radius;
		Frame.SunRadiance = sun.Radiance;
		Frame.Flags |= gpu::FrameFlag_SunEnabled;
	}
	if (SkyLightIndex != FRT_INVALID_INDEX)
	{
		Frame.Flags |= gpu::FrameFlag_SkyEnabled;
	}
}

D3D12_GPU_VIRTUAL_ADDRESS CGpuScene::GetTlasAddress () const
{
	return Tlas[CurrentTlas].GetGpuAddress();
}
}
