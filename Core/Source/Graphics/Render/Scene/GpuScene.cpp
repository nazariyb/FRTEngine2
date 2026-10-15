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
	SkyPrefilterPipeline = Pipelines->CreateCompute("SkyPrefilter", MakeShaderDesc("Scene/SkyBake.hlsl", "PrefilterSpecular", "cs_6_6"));
	SkyIrradiancePipeline = Pipelines->CreateCompute("SkyIrradiance", MakeShaderDesc("Scene/SkyBake.hlsl", "ConvolveIrradiance", "cs_6_6"));

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

	// Image-based lighting for the raster preview: one prefiltered slice per roughness step,
	// in a 3D texture so that trilinear filtering also interpolates between roughness levels.
	rhi::STextureDesc prefiltered;
	prefiltered.Width = SkyPrefilterSize;
	prefiltered.Height = SkyPrefilterSize;
	prefiltered.DepthOrArraySize = static_cast<uint16>(SkyPrefilterSlices);
	prefiltered.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	prefiltered.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	prefiltered.bUav = true;
	prefiltered.Name = "SkyPrefiltered";
	SkyPrefiltered = Device->CreateTexture(prefiltered);

	rhi::STextureDesc irradiance;
	irradiance.Width = SkyIrradianceSize;
	irradiance.Height = SkyIrradianceSize;
	irradiance.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	irradiance.bUav = true;
	irradiance.Name = "SkyIrradiance";
	SkyIrradiance = Device->CreateTexture(irradiance);
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
	Device->Destroy(SkyPrefiltered);
	Device->Destroy(SkyIrradiance);
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

		// Per-section surface area and centroid, for the raster preview's energy balance and
		// emitter stand-ins (instances scale them; the triangles themselves stay on the GPU).
		entry.SectionArea.assign(Model.Sections.Count(), 0.0f);
		entry.SectionCentroid.assign(Model.Sections.Count(), DirectX::XMFLOAT3{});
		entry.SectionBoundsMin.assign(Model.Sections.Count(), DirectX::XMFLOAT3{});
		entry.SectionBoundsMax.assign(Model.Sections.Count(), DirectX::XMFLOAT3{});
		for (uint32 s = 0; s < Model.Sections.Count(); ++s)
		{
			const SRenderSection& section = Model.Sections[s];
			double area = 0.0, cx = 0.0, cy = 0.0, cz = 0.0;
			DirectX::XMFLOAT3 smin = { 1e30f, 1e30f, 1e30f };
			DirectX::XMFLOAT3 smax = { -1e30f, -1e30f, -1e30f };
			for (uint32 t = 0; t + 2u < section.IndexCount; t += 3u)
			{
				const uint32 base = section.IndexOffset + t;
				if (base + 2u >= Model.Indices.Count())
				{
					break;
				}
				const Vector3f& a = Model.Vertices[Model.Indices[base + 0u] + section.VertexOffset].Position;
				const Vector3f& b = Model.Vertices[Model.Indices[base + 1u] + section.VertexOffset].Position;
				const Vector3f& c = Model.Vertices[Model.Indices[base + 2u] + section.VertexOffset].Position;
				const float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
				const float e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
				const float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
				const double triArea = 0.5 * std::sqrt(static_cast<double>(nx) * nx + static_cast<double>(ny) * ny + static_cast<double>(nz) * nz);
				for (const Vector3f* v : { &a, &b, &c })
				{
					smin = { (std::min)(smin.x, v->x), (std::min)(smin.y, v->y), (std::min)(smin.z, v->z) };
					smax = { (std::max)(smax.x, v->x), (std::max)(smax.y, v->y), (std::max)(smax.z, v->z) };
				}
				area += triArea;
				cx += triArea * (a.x + b.x + c.x) / 3.0;
				cy += triArea * (a.y + b.y + c.y) / 3.0;
				cz += triArea * (a.z + b.z + c.z) / 3.0;
			}
			entry.SectionArea[s] = static_cast<float>(area);
			entry.SectionBoundsMin[s] = smax.x >= smin.x ? smin : entry.BoundsMin;
			entry.SectionBoundsMax[s] = smax.x >= smin.x ? smax : entry.BoundsMax;
			if (area > 0.0)
			{
				entry.SectionCentroid[s] = { static_cast<float>(cx / area), static_cast<float>(cy / area), static_cast<float>(cz / area) };
			}
		}

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
		SceneMin = sceneMin;
		SceneMax = sceneMax;
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
			draw.bAlphaTested = material && material->AlphaMode == EAlphaMode::Mask;
			draw.bHelper = FrameInstances[i].bHelper;
			const bool bSectionBounds = s < entry.SectionBoundsMin.size();
			const DirectX::XMFLOAT3 lo = bSectionBounds ? entry.SectionBoundsMin[s] : entry.BoundsMin;
			const DirectX::XMFLOAT3 hi = bSectionBounds ? entry.SectionBoundsMax[s] : entry.BoundsMax;
			draw.BoundsMin = { 1e30f, 1e30f, 1e30f };
			draw.BoundsMax = { -1e30f, -1e30f, -1e30f };
			for (uint32 c = 0; c < 8u; ++c)
			{
				const DirectX::XMFLOAT3 p = TransformPoint(FrameInstances[i].ObjectToWorld, (c & 1u) ? hi.x : lo.x, (c & 2u) ? hi.y : lo.y, (c & 4u) ? hi.z : lo.z);
				draw.BoundsMin = { (std::min)(draw.BoundsMin.x, p.x), (std::min)(draw.BoundsMin.y, p.y), (std::min)(draw.BoundsMin.z, p.z) };
				draw.BoundsMax = { (std::max)(draw.BoundsMax.x, p.x), (std::max)(draw.BoundsMax.y, p.y), (std::max)(draw.BoundsMax.z, p.z) };
			}
			Draws.push_back(draw);
		}
	}
	UpdateRasterLighting();

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

	// Image-based lighting for the raster preview (which cannot trace the sky): radiance
	// pre-integrated over GGX lobes per roughness (split-sum) and cosine-convolved irradiance.
	Context.Barriers.Transition(SkyPrefiltered, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Context.Barriers.Transition(SkyIrradiance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	{
		const uint32 filter[4] = { SkyEnvironment.Srv, SkyPrefiltered.Uav, SkyPrefilterSize, SkyPrefilterSlices };
		Context.SetRootConstants(filter, 4u);
		Context.Dispatch(SkyPrefilterPipeline, rhi::DivideRoundUp(SkyPrefilterSize, 8u), rhi::DivideRoundUp(SkyPrefilterSize, 8u), SkyPrefilterSlices);
	}
	{
		const uint32 filter[4] = { SkyEnvironment.Srv, SkyIrradiance.Uav, SkyIrradianceSize, 1u };
		Context.SetRootConstants(filter, 4u);
		Context.Dispatch2D(SkyIrradiancePipeline, SkyIrradianceSize, SkyIrradianceSize);
	}
	Context.Barriers.Transition(SkyPrefiltered, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Context.Barriers.Transition(SkyIrradiance, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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

void CGpuScene::UpdateRasterLighting ()
{
	// Per instance and section: world area (object area scaled by |det|^(2/3), exact for rotation
	// and uniform scale), mean reflectance from the material factors and the textures' average
	// texels, emitted power. Textures that are still streaming count as white until resident.
	RasterEmitters.clear();
	struct SSurface
	{
		float Area = 0.0f;                  // world area x opacity coverage
		float Reflectance[3] = {};
		DirectX::XMFLOAT3 BoundsMin = {};
		DirectX::XMFLOAT3 BoundsMax = {};
	};
	std::vector<SSurface> surfaces;
	double surfaceArea = 0.0;
	double flux[3] = {};
	double surfaceCentroid[3] = {};
	for (size_t i = 0; i < FrameInstances.size(); ++i)
	{
		const SSceneInstance& instance = FrameInstances[i];
		const auto modelIt = Models.find(instance.Model);
		if (instance.bHelper || modelIt == Models.end())
		{
			continue;
		}
		const SModelEntry& entry = modelIt->second;
		const SRenderModel& model = *instance.Model;
		const DirectX::XMFLOAT3X4& xf = instance.ObjectToWorld;
		const float det =
			xf.m[0][0] * (xf.m[1][1] * xf.m[2][2] - xf.m[1][2] * xf.m[2][1]) -
			xf.m[0][1] * (xf.m[1][0] * xf.m[2][2] - xf.m[1][2] * xf.m[2][0]) +
			xf.m[0][2] * (xf.m[1][0] * xf.m[2][1] - xf.m[1][1] * xf.m[2][0]);
		const float areaScale = std::pow(std::abs(det), 2.0f / 3.0f);

		double emitterArea = 0.0;
		double emitterFlux[3] = {};
		double emitterCentroid[3] = {};
		const uint32 sections = (std::min)({ entry.SectionCount, static_cast<uint32>(entry.SectionArea.size()), model.Sections.Count() });
		for (uint32 s = 0; s < sections; ++s)
		{
			if (entry.GeometryBase + s >= GeometryTable.size())
			{
				break;
			}
			const gpu::SGeometryData& g = GeometryTable[entry.GeometryBase + s];
			const gpu::SMaterialData& m = MaterialTable[(std::min)(g.MaterialIndex, static_cast<uint32>(MaterialTable.size() - 1u))];
			const uint32 mi = model.Sections[s].MaterialIndex;
			const SMaterial* material = mi < model.Materials.Count() ? model.Materials[mi].GetRawIgnoringLifetime() : nullptr;
			DirectX::XMFLOAT4 baseAvg = { 1.0f, 1.0f, 1.0f, 1.0f };
			DirectX::XMFLOAT4 mrAvg = { 1.0f, 1.0f, 1.0f, 1.0f };
			DirectX::XMFLOAT4 emissiveAvg = { 1.0f, 1.0f, 1.0f, 1.0f };
			if (const auto materialIt = material ? MaterialEntries.find(material) : MaterialEntries.end(); materialIt != MaterialEntries.end())
			{
				baseAvg = Textures.GetAverageColor(materialIt->second.BaseColor);
				mrAvg = Textures.GetAverageColor(materialIt->second.MetallicRoughness);
				emissiveAvg = Textures.GetAverageColor(materialIt->second.Emissive);
			}
			const float area = entry.SectionArea[s] * areaScale;
			// Masked and blended surfaces only intercept the light their opacity covers.
			const bool bCutout = (m.Flags & (gpu::MaterialFlag_AlphaMask | gpu::MaterialFlag_AlphaBlend)) != 0u;
			const float coverage = bCutout ? std::clamp(m.BaseColorFactor.w * baseAvg.w, 0.0f, 1.0f) : 1.0f;
			const float metallic = std::clamp(m.Metallic * mrAvg.z, 0.0f, 1.0f);
			// Hemispherical reflectance of the glTF model: Fresnel-averaged dielectric coat over
			// the diffuse base, mixed with the metal (whose reflectance is its base colour).
			const float f0 = 0.04f * m.SpecularFactor;
			const float coat = f0 + (1.0f - f0) / 21.0f;
			const float base[3] = { m.BaseColorFactor.x * baseAvg.x, m.BaseColorFactor.y * baseAvg.y, m.BaseColorFactor.z * baseAvg.z };
			SSurface surface;
			surface.Area = area * coverage;
			for (uint32 c = 0; c < 3u; ++c)
			{
				const float b = std::clamp(base[c], 0.0f, 1.0f);
				surface.Reflectance[c] = (1.0f - metallic) * (b * (1.0f - coat) + coat) + metallic * b;
			}
			{
				const DirectX::XMFLOAT3& oc = entry.SectionCentroid[s];
				const DirectX::XMFLOAT3 wc = TransformPoint(xf, oc.x, oc.y, oc.z);
				surfaceArea += surface.Area;
				surfaceCentroid[0] += surface.Area * wc.x;
				surfaceCentroid[1] += surface.Area * wc.y;
				surfaceCentroid[2] += surface.Area * wc.z;
				const bool bBounds = s < entry.SectionBoundsMin.size();
				const DirectX::XMFLOAT3 lo = bBounds ? entry.SectionBoundsMin[s] : entry.BoundsMin;
				const DirectX::XMFLOAT3 hi = bBounds ? entry.SectionBoundsMax[s] : entry.BoundsMax;
				surface.BoundsMin = { 1e30f, 1e30f, 1e30f };
				surface.BoundsMax = { -1e30f, -1e30f, -1e30f };
				for (uint32 corner = 0; corner < 8u; ++corner)
				{
					const DirectX::XMFLOAT3 w = TransformPoint(xf, (corner & 1u) ? hi.x : lo.x, (corner & 2u) ? hi.y : lo.y, (corner & 4u) ? hi.z : lo.z);
					surface.BoundsMin = { (std::min)(surface.BoundsMin.x, w.x), (std::min)(surface.BoundsMin.y, w.y), (std::min)(surface.BoundsMin.z, w.z) };
					surface.BoundsMax = { (std::max)(surface.BoundsMax.x, w.x), (std::max)(surface.BoundsMax.y, w.y), (std::max)(surface.BoundsMax.z, w.z) };
				}
			}
			surfaces.push_back(surface);
			if ((g.Flags & gpu::GeometryFlag_Emissive) != 0u)
			{
				// Same convention as the path tracer's emissive triangles: pi * L per unit area.
				const float emitted[3] = { m.EmissiveFactor.x * emissiveAvg.x, m.EmissiveFactor.y * emissiveAvg.y, m.EmissiveFactor.z * emissiveAvg.z };
				const DirectX::XMFLOAT3& oc = entry.SectionCentroid[s];
				const DirectX::XMFLOAT3 wc = TransformPoint(xf, oc.x, oc.y, oc.z);
				emitterArea += area;
				emitterCentroid[0] += area * wc.x;
				emitterCentroid[1] += area * wc.y;
				emitterCentroid[2] += area * wc.z;
				for (uint32 c = 0; c < 3u; ++c)
				{
					emitterFlux[c] += kPi * emitted[c] * area;
				}
			}
		}
		if (emitterArea > 0.0)
		{
			SRasterEmitter emitter;
			emitter.Position = {
				static_cast<float>(emitterCentroid[0] / emitterArea),
				static_cast<float>(emitterCentroid[1] / emitterArea),
				static_cast<float>(emitterCentroid[2] / emitterArea) };
			// A sphere of the same area and power: radius from 4 pi r^2 = A, radiance = flux / (pi A).
			emitter.Radius = static_cast<float>(std::sqrt(emitterArea / (4.0 * kPi)));
			emitter.Radiance = {
				static_cast<float>(emitterFlux[0] / (kPi * emitterArea)),
				static_cast<float>(emitterFlux[1] / (kPi * emitterArea)),
				static_cast<float>(emitterFlux[2] / (kPi * emitterArea)) };
			emitter.Power = Luminance(static_cast<float>(emitterFlux[0]), static_cast<float>(emitterFlux[1]), static_cast<float>(emitterFlux[2]));
			emitter.InstanceIndex = static_cast<uint32>(i);
			if (emitter.Power > 0.0f)
			{
				RasterEmitters.push_back(emitter);
			}
		}
	}
	std::stable_sort(RasterEmitters.begin(), RasterEmitters.end(),
		[] (const SRasterEmitter& A, const SRasterEmitter& B) { return A.Power > B.Power; });

	// Only a scene that declares portals says it is closed: then the sun and the sky enter
	// through those openings alone, and the openings absorb whatever reaches them from inside.
	RasterAmbient = {};
	RasterAmbient.bEnclosed = !FramePortals.empty();
	if (!RasterAmbient.bEnclosed)
	{
		return;
	}
	const SSkyConstants& sky = FrameSky.Constants;
	const double surfaceWeight = surfaceArea > 0.0 ? 1.0 / surfaceArea : 0.0;
	// Surfaces and emitters entirely on the outer side of an opening are not in the enclosure
	// (the shading pass makes the same split per pixel): a roof does not absorb interior light.
	auto isOutside = [&] (const DirectX::XMFLOAT3& Lo, const DirectX::XMFLOAT3& Hi)
	{
		for (const gpu::SPortalData& portal : RasterAmbient.Portals)
		{
			bool bAllOutside = true;
			for (uint32 corner = 0; corner < 8u && bAllOutside; ++corner)
			{
				const float x = (corner & 1u) ? Hi.x : Lo.x, y = (corner & 2u) ? Hi.y : Lo.y, z = (corner & 4u) ? Hi.z : Lo.z;
				bAllOutside = (x - portal.Center.x) * portal.Normal.x + (y - portal.Center.y) * portal.Normal.y + (z - portal.Center.z) * portal.Normal.z < 0.0f;
			}
			if (bAllOutside)
			{
				return true;
			}
		}
		return false;
	};
	const float inside[3] = {
		static_cast<float>(surfaceCentroid[0] * surfaceWeight),
		static_cast<float>(surfaceCentroid[1] * surfaceWeight),
		static_cast<float>(surfaceCentroid[2] * surfaceWeight) };
	for (gpu::SPortalData portal : FramePortals)
	{
		const float side =
			(inside[0] - portal.Center.x) * portal.Normal.x +
			(inside[1] - portal.Center.y) * portal.Normal.y +
			(inside[2] - portal.Center.z) * portal.Normal.z;
		if (side < 0.0f)
		{
			portal.Normal = { -portal.Normal.x, -portal.Normal.y, -portal.Normal.z };
		}
		RasterAmbient.Portals.push_back(portal);
	}
	surfaceArea = 0.0;
	double reflectedArea[3] = {};
	for (const SSurface& surface : surfaces)
	{
		if (isOutside(surface.BoundsMin, surface.BoundsMax))
		{
			continue;
		}
		surfaceArea += surface.Area;
		for (uint32 c = 0; c < 3u; ++c)
		{
			reflectedArea[c] += surface.Area * surface.Reflectance[c];
		}
	}
	for (const SRasterEmitter& emitter : RasterEmitters)
	{
		if (!isOutside(emitter.Position, emitter.Position))
		{
			// Flux of the stand-in sphere: pi * L * 4 pi r^2 (the emitting area).
			const float sphereArea = 4.0f * kPi * emitter.Radius * emitter.Radius;
			flux[0] += kPi * emitter.Radiance.x * sphereArea;
			flux[1] += kPi * emitter.Radiance.y * sphereArea;
			flux[2] += kPi * emitter.Radiance.z * sphereArea;
		}
	}
	for (const gpu::SPortalData& portal : RasterAmbient.Portals)
	{
		surfaceArea += portal.Area;
		if (SunLightIndex != FRT_INVALID_INDEX)
		{
			const gpu::SLightData& sun = LightTable[SunLightIndex];
			// The sun must be on the outer side (portal normals face the interior).
			const float c = -(portal.Normal.x * sun.AxisU.x + portal.Normal.y * sun.AxisU.y + portal.Normal.z * sun.AxisU.z);
			if (c > 0.0f)
			{
				flux[0] += sun.Radiance.x * sun.SolidAngle * portal.Area * c;
				flux[1] += sun.Radiance.y * sun.SolidAngle * portal.Area * c;
				flux[2] += sun.Radiance.z * sun.SolidAngle * portal.Area * c;
			}
		}
		if (SkyLightIndex != FRT_INVALID_INDEX)
		{
			// Sky irradiance on the opening from outside: cosine-weighted integral over the
			// sphere (rows uniform in y = uniform in solid angle), facing away from the interior.
			const int rows = 32, columns = 32;
			const double cell = 4.0 * kPi / (rows * columns);
			double e[3] = {};
			for (int r = 0; r < rows; ++r)
			{
				const float y = -1.0f + (2.0f * r + 1.0f) / rows;
				const float ring = std::sqrt((std::max)(0.0f, 1.0f - y * y));
				const DirectX::XMFLOAT3 radiance = EvalProceduralSkyCpu(sky, y);
				for (int k = 0; k < columns; ++k)
				{
					const float phi = 2.0f * kPi * (k + 0.5f) / columns;
					const float cosine = -(portal.Normal.x * ring * std::cos(phi) + portal.Normal.y * y + portal.Normal.z * ring * std::sin(phi));
					if (cosine > 0.0f)
					{
						e[0] += radiance.x * cosine * cell;
						e[1] += radiance.y * cosine * cell;
						e[2] += radiance.z * cosine * cell;
					}
				}
			}
			for (uint32 c = 0; c < 3u; ++c)
			{
				flux[c] += e[c] * portal.Area;
			}
		}
	}
	RasterAmbient.Area = static_cast<float>(surfaceArea);
	for (uint32 c = 0; c < 3u; ++c)
	{
		const double albedo = surfaceArea > 0.0 ? (std::min)(reflectedArea[c] / surfaceArea, 0.98) : 0.0;
		const double radiance = surfaceArea > 0.0 ? flux[c] * albedo / (1.0 - albedo) / (kPi * surfaceArea) : 0.0;
		(&RasterAmbient.Albedo.x)[c] = static_cast<float>(albedo);
		(&RasterAmbient.Flux.x)[c] = static_cast<float>(flux[c]);
		(&RasterAmbient.Radiance.x)[c] = static_cast<float>(radiance);
	}
	const float luminance = Luminance(RasterAmbient.Radiance.x, RasterAmbient.Radiance.y, RasterAmbient.Radiance.z);
	if (std::abs(luminance - LoggedAmbientLuminance) > 0.01f * (std::max)(luminance, 1e-12f))
	{
		LoggedAmbientLuminance = luminance;
		std::printf("[raster] enclosure: area %.1f m2, albedo %.3f %.3f %.3f, flux %.2f %.2f %.2f W, ambient radiance %.3g %.3g %.3g, %zu emitter(s)\n",
			RasterAmbient.Area, RasterAmbient.Albedo.x, RasterAmbient.Albedo.y, RasterAmbient.Albedo.z,
			RasterAmbient.Flux.x, RasterAmbient.Flux.y, RasterAmbient.Flux.z,
			RasterAmbient.Radiance.x, RasterAmbient.Radiance.y, RasterAmbient.Radiance.z, RasterEmitters.size());
		for (const gpu::SPortalData& portal : RasterAmbient.Portals)
		{
			std::printf("[raster] portal at %.2f %.2f %.2f, interior normal %.2f %.2f %.2f, %.2f m2\n",
				portal.Center.x, portal.Center.y, portal.Center.z, portal.Normal.x, portal.Normal.y, portal.Normal.z, portal.Area);
		}
	}
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
