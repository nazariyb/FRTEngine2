#include "Graphics/Render/Passes/PathTracer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "imgui.h"

#include "Profiler/Profiler.h"


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

rhi::STextureDesc MakeTarget (uint32 Width, uint32 Height, DXGI_FORMAT Format, const wchar_t* Name)
{
	rhi::STextureDesc desc;
	desc.Width = Width;
	desc.Height = Height;
	desc.Format = Format;
	desc.bUav = true;
	desc.bSrv = true;
	desc.InitialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	desc.Name = Name;
	return desc;
}
}


void CPathTracer::Initialize (rhi::CDevice& InDevice, rhi::CPipelineLibrary& InPipelines)
{
	Device = &InDevice;
	Pipelines = &InPipelines;

	rhi::SShaderCompileDesc base = MakeShaderDesc("PathTracing/PathTracer.hlsl", "", "lib_6_8", { { "FRT_SER", "0" } });
	BaseVariant.Library = Pipelines->RegisterLibrary("PathTracer", base);

	if (Device->GetCaps().bShaderExecutionReordering)
	{
		rhi::SShaderCompileDesc ser = MakeShaderDesc("PathTracing/PathTracer.hlsl", "", "lib_6_9", { { "FRT_SER", "1" } });
		// With payload access qualifiers the lib_6_9 state object never makes closest-hit writes
		// visible to the caller (plain TraceRay returns the caller's initial payload; observed on
		// RTX 40 / Agility 1.619). The payload is tiny, so qualifiers buy nothing here anyway.
		ser.ExtraArguments.push_back(L"-disable-payload-qualifiers");
		ser.ExtraArguments.push_back(L"-Wno-payload-access-qualifier"); // the [raypayload] annotations are intentionally ignored here
		SerVariant.Library = Pipelines->RegisterLibrary("PathTracer(SER)", ser);
	}

	RemodulatePipeline = Pipelines->CreateCompute("PathTracer_Remodulate", MakeShaderDesc("PathTracing/Remodulate.hlsl", "main", "cs_6_6"));
	RadianceCache.Initialize(InDevice, InPipelines);
}

void CPathTracer::DestroyTargets ()
{
	for (rhi::SGpuTexture* t : { &Targets.Diffuse, &Targets.Specular, &Targets.NormalRoughness, &Targets.ViewZ,
		&Targets.MotionVectors, &Targets.DiffuseAlbedo, &Targets.SpecularAlbedo, &Targets.Emission, &Targets.Composite,
		&Targets.Accumulation, &Targets.Debug })
	{
		if (t->IsValid())
		{
			Device->Destroy(*t);
		}
	}
}

void CPathTracer::Shutdown ()
{
	if (!Device)
	{
		return;
	}
	DestroyTargets();
	RadianceCache.Shutdown();
	for (SPipelineVariant* v : { &SerVariant, &BaseVariant })
	{
		if (v->StateObject)
		{
			Device->DeferRelease(rhi::ComPtr<IUnknown>(v->StateObject));
			v->StateObject.Reset();
		}
		Device->Destroy(v->ShaderTable);
	}
	Device = nullptr;
}

void CPathTracer::Resize (uint32 InWidth, uint32 InHeight)
{
	if (InWidth == Width && InHeight == Height && Targets.Diffuse.IsValid())
	{
		return;
	}
	DestroyTargets();
	Width = (std::max)(InWidth, 1u);
	Height = (std::max)(InHeight, 1u);
	Targets.Diffuse = Device->CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"PT_Diffuse"));
	Targets.Specular = Device->CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"PT_Specular"));
	Targets.NormalRoughness = Device->CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"PT_NormalRoughness"));
	Targets.ViewZ = Device->CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R32_FLOAT, L"PT_ViewZ"));
	Targets.MotionVectors = Device->CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R16G16_FLOAT, L"PT_MotionVectors"));
	Targets.DiffuseAlbedo = Device->CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R8G8B8A8_UNORM, L"PT_DiffuseAlbedo"));
	Targets.SpecularAlbedo = Device->CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"PT_SpecularAlbedo"));
	Targets.Emission = Device->CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"PT_Emission"));
	Targets.Composite = Device->CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"PT_Composite"));
	Targets.Accumulation = Device->CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R32G32B32A32_FLOAT, L"PT_Accumulation"));
	Targets.Debug = Device->CreateTexture(MakeTarget(Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"PT_Debug"));
}

bool CPathTracer::BuildVariant (SPipelineVariant& Variant, const wchar_t* Name, ID3D12GraphicsCommandList* CommandList)
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

bool CPathTracer::IsReady () const
{
	return BaseVariant.bValid || SerVariant.bValid;
}

bool CPathTracer::IsSerActive () const
{
	return Settings.bShaderExecutionReordering && SerVariant.bValid;
}

void CPathTracer::UpdatePipelines (ID3D12GraphicsCommandList* CommandList)
{
	for (SPipelineVariant* variant : { &BaseVariant, &SerVariant })
	{
		if (variant->Library == rhi::InvalidPipelineHandle)
		{
			continue;
		}
		const uint32 version = Pipelines->GetLibraryVersion(variant->Library);
		if (version != variant->BuiltVersion && Pipelines->GetLibrary(variant->Library))
		{
			variant->BuiltVersion = version;
			BuildVariant(*variant, variant == &SerVariant ? L"PathTracer(SER)" : L"PathTracer", CommandList);
		}
	}
}

void CPathTracer::Dispatch (CRenderContext& Context, bool bReference, bool bAccumulate)
{
	UpdatePipelines(Context.CommandList);

	SPipelineVariant* variant = IsSerActive() ? &SerVariant : (BaseVariant.bValid ? &BaseVariant : nullptr);
	if (!variant || !Targets.Diffuse.IsValid())
	{
		return;
	}

	gpu::SPathTracerConstants c = {};
	c.SamplesPerPixel = (std::max)(Settings.SamplesPerPixel, 1u);
	c.MaxBounces = Settings.MaxBounces;
	c.RussianRouletteDepth = Settings.RussianRouletteDepth;
	c.RisCandidates = (std::clamp)(Settings.RisCandidates, 1u, 32u);
	c.RisCandidatesSecondary = (std::clamp)(Settings.RisCandidatesSecondary, 1u, 32u);
	c.IndirectClamp = bReference ? 0.0f : Settings.IndirectClamp;
	c.MinRoughness = Settings.MinRoughness;
	c.DebugMode = Settings.DebugMode;
	c.Flags = 0u;
	if (variant == &SerVariant) { c.Flags |= gpu::PathFlag_Reorder; }
	if (!bReference) { c.Flags |= gpu::PathFlag_SplitLobes; }
	if (Settings.bPortalPreFilter) { c.Flags |= gpu::PathFlag_PortalPreFilter; }
	if (Settings.bPathRegularization && !bReference) { c.Flags |= gpu::PathFlag_Regularize; }
	if (Settings.bDisableNee) { c.Flags |= gpu::PathFlag_DisableNee; }
	if (Settings.bDisableBsdfMis) { c.Flags |= gpu::PathFlag_DisableBsdfMis; }
	if (bAccumulate) { c.Flags |= gpu::PathFlag_Accumulate; }
	if (Settings.bNormalMaps) { c.Flags |= gpu::PathFlag_NormalMaps; }
	if (Settings.bWhiteFurnace) { c.Flags |= gpu::PathFlag_WhiteFurnace; }
	if (Settings.bPortalGuiding) { c.Flags |= gpu::PathFlag_PortalGuiding; }
	if (Settings.bInlineRayQueries) { c.Flags |= gpu::PathFlag_InlineBounces; }
	// Validation modes measure the plain estimator: no queries, and no updates either, since the
	// update paths would fill the cells with that mode's radiance (white furnace: 3x the real
	// lighting on average in the demo view) and serve it for many frames afterwards.
	const bool bValidation = Settings.bWhiteFurnace || Settings.bDisableNee || Settings.bDisableBsdfMis;
	if (Settings.bPrimarySurfaceReplacement && !bReference && !bValidation) { c.Flags |= gpu::PathFlag_PrimarySurfaceReplacement; }
	c.OutDiffuse = Targets.Diffuse.Uav;
	c.OutSpecular = Targets.Specular.Uav;
	c.OutNormalRoughness = Targets.NormalRoughness.Uav;
	c.OutViewZ = Targets.ViewZ.Uav;
	c.OutMotionVectors = Targets.MotionVectors.Uav;
	c.OutDiffuseAlbedo = Targets.DiffuseAlbedo.Uav;
	c.OutSpecularAlbedo = Targets.SpecularAlbedo.Uav;
	c.OutEmission = Targets.Emission.Uav;
	c.OutAccumulation = Targets.Accumulation.Uav;
	c.OutDebug = Settings.DebugMode != gpu::PathDebug_None ? Targets.Debug.Uav : FRT_INVALID_INDEX;
	c.OutComposite = Targets.Composite.Uav;
	c.Cache.KeyBuffer = c.Cache.AccumulationBuffer = c.Cache.ResolvedBuffer = FRT_INVALID_INDEX;

	for (rhi::SGpuTexture* t : { &Targets.Diffuse, &Targets.Specular, &Targets.NormalRoughness, &Targets.ViewZ,
		&Targets.MotionVectors, &Targets.DiffuseAlbedo, &Targets.SpecularAlbedo, &Targets.Emission, &Targets.Composite,
		&Targets.Accumulation, &Targets.Debug })
	{
		Context.Barriers.Transition(*t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	Context.FlushBarriers();

	Context.BindCompute();
	// (Validation modes, see above.) Idle also clears the cache when real-time rendering resumes.
	if (!bReference && !bValidation)
	{
		RadianceCache.Prepare(Context, c.Cache);
		RadianceCache.Update(Context, c, Width, Height);
	}
	else
	{
		RadianceCache.MarkIdle();
	}
	Context.PushPassConstants(c);

	{
		// Scope name kept for the profiling pipeline (MetricsAggregator reads "RT_Dispatch").
		FRT_GPU_SCOPE(Context.CommandList, "RT_Dispatch");
		D3D12_DISPATCH_RAYS_DESC desc = variant->DispatchTemplate;
		desc.Width = Width;
		desc.Height = Height;
		Context.CommandList->SetPipelineState1(variant->StateObject.Get());
		Context.CommandList->DispatchRays(&desc);
	}

	for (rhi::SGpuTexture* t : { &Targets.Diffuse, &Targets.Specular, &Targets.NormalRoughness, &Targets.ViewZ,
		&Targets.MotionVectors, &Targets.DiffuseAlbedo, &Targets.SpecularAlbedo, &Targets.Emission, &Targets.Composite,
		&Targets.Accumulation, &Targets.Debug })
	{
		Context.Barriers.Transition(*t, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	Context.FlushBarriers();
}

void CPathTracer::Remodulate (CRenderContext& Context, rhi::SGpuTexture& DiffuseIn, rhi::SGpuTexture& SpecularIn, rhi::SGpuTexture& Output)
{
	FRT_GPU_SCOPE(Context.CommandList, "PT_Remodulate");
	const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	Context.Barriers.Transition(DiffuseIn, read);
	Context.Barriers.Transition(SpecularIn, read);
	Context.Barriers.Transition(Targets.DiffuseAlbedo, read);
	Context.Barriers.Transition(Targets.SpecularAlbedo, read);
	Context.Barriers.Transition(Targets.Emission, read);
	Context.Barriers.Transition(Output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Context.BindCompute();
	const uint32 constants[8] = {
		DiffuseIn.Srv, SpecularIn.Srv, Targets.DiffuseAlbedo.Srv, Targets.SpecularAlbedo.Srv,
		Targets.Emission.Srv, Output.Uav, Width, Height };
	Context.FlushBarriers();
	Context.SetRootConstants(constants, 8u);
	Context.Dispatch2D(RemodulatePipeline, Width, Height);
	Context.Barriers.Transition(Output, read);
	Context.FlushBarriers();
}

void CPathTracer::DrawSettingsUi ()
{
	int spp = static_cast<int>(Settings.SamplesPerPixel);
	if (ImGui::SliderInt("Samples / pixel", &spp, 1, 64)) { Settings.SamplesPerPixel = static_cast<uint32>(spp); }
	int bounces = static_cast<int>(Settings.MaxBounces);
	if (ImGui::SliderInt("Max bounces", &bounces, 0, 32)) { Settings.MaxBounces = static_cast<uint32>(bounces); }
	int rr = static_cast<int>(Settings.RussianRouletteDepth);
	if (ImGui::SliderInt("Russian roulette from", &rr, 1, 32)) { Settings.RussianRouletteDepth = static_cast<uint32>(rr); }
	int ris = static_cast<int>(Settings.RisCandidates);
	if (ImGui::SliderInt("RIS light candidates", &ris, 1, 32)) { Settings.RisCandidates = static_cast<uint32>(ris); }
	int ris2 = static_cast<int>(Settings.RisCandidatesSecondary);
	if (ImGui::SliderInt("RIS candidates (bounces)", &ris2, 1, 32)) { Settings.RisCandidatesSecondary = static_cast<uint32>(ris2); }
	ImGui::SliderFloat("Indirect clamp (0 = off)", &Settings.IndirectClamp, 0.0f, 100.0f, "%.1f");
	ImGui::SliderFloat("Min roughness", &Settings.MinRoughness, 0.0f, 0.5f, "%.3f");
	ImGui::Checkbox("Normal maps", &Settings.bNormalMaps);
	ImGui::Checkbox("Path regularisation (real time)", &Settings.bPathRegularization);
	ImGui::Checkbox("Portal-guided sky sampling", &Settings.bPortalGuiding);
	ImGui::Checkbox("Legacy portal pre-filter", &Settings.bPortalPreFilter);
	ImGui::Checkbox("Primary surface replacement (mirrors)", &Settings.bPrimarySurfaceReplacement);
	ImGui::BeginDisabled(!SerVariant.bValid);
	ImGui::Checkbox("Shader execution reordering", &Settings.bShaderExecutionReordering);
	ImGui::EndDisabled();
	if (!SerVariant.bValid)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("(unavailable)");
	}
	if (ImGui::TreeNode("Validation"))
	{
		ImGui::Checkbox("BSDF sampling only (no NEE)", &Settings.bDisableNee);
		ImGui::Checkbox("NEE only for emitters", &Settings.bDisableBsdfMis);
		ImGui::Checkbox("White furnace", &Settings.bWhiteFurnace);
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Radiance cache (real time)"))
	{
		RadianceCache.DrawSettingsUi();
		ImGui::TreePop();
	}
	static const char* kDebugModes[] = {
		"None", "Base colour", "Shading normal", "Geometric normal", "Roughness", "Metallic", "Emission",
		"Motion vectors", "View Z", "Path length", "Direct only", "Indirect only", "UV", "Instance / section", "First bounce hit",
		"Radiance cache", "Radiance cache cells" };
	int mode = static_cast<int>(Settings.DebugMode);
	if (ImGui::Combo("Debug view", &mode, kDebugModes, static_cast<int>(std::size(kDebugModes))))
	{
		Settings.DebugMode = static_cast<uint32>(mode);
	}
}
}
