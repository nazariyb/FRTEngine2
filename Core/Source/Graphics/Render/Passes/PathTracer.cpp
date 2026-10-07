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


void CPathTracer::Initialize (rhi::CDevice& InDevice, rhi::CPipelineLibrary& InPipelines, const CPathTracerPrograms& InPrograms)
{
	Device = &InDevice;
	Programs = &InPrograms;
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

bool CPathTracer::IsReady () const
{
	return Programs && Programs->IsReady();
}

bool CPathTracer::IsSerActive () const
{
	return Settings.bShaderExecutionReordering && Programs && Programs->IsSerAvailable();
}

void CPathTracer::Dispatch (CRenderContext& Context, bool bReference, bool bAccumulate)
{
	const CPathTracerPrograms::SVariant* variant = Programs ? Programs->Select(Settings.bShaderExecutionReordering) : nullptr;
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
	if (variant->bReorder) { c.Flags |= gpu::PathFlag_Reorder; }
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
	Context.Dispatch2D(Programs->GetRemodulatePipeline(), Width, Height);
	Context.Barriers.Transition(Output, read);
	Context.FlushBarriers();
}

void CPathTracer::DrawSettingsUi (SPathTracerSettings& Edit, SRadianceCacheSettings& EditCache)
{
	int spp = static_cast<int>(Edit.SamplesPerPixel);
	if (ImGui::SliderInt("Samples / pixel", &spp, 1, 64)) { Edit.SamplesPerPixel = static_cast<uint32>(spp); }
	int bounces = static_cast<int>(Edit.MaxBounces);
	if (ImGui::SliderInt("Max bounces", &bounces, 0, 32)) { Edit.MaxBounces = static_cast<uint32>(bounces); }
	int rr = static_cast<int>(Edit.RussianRouletteDepth);
	if (ImGui::SliderInt("Russian roulette from", &rr, 1, 32)) { Edit.RussianRouletteDepth = static_cast<uint32>(rr); }
	int ris = static_cast<int>(Edit.RisCandidates);
	if (ImGui::SliderInt("RIS light candidates", &ris, 1, 32)) { Edit.RisCandidates = static_cast<uint32>(ris); }
	int ris2 = static_cast<int>(Edit.RisCandidatesSecondary);
	if (ImGui::SliderInt("RIS candidates (bounces)", &ris2, 1, 32)) { Edit.RisCandidatesSecondary = static_cast<uint32>(ris2); }
	ImGui::SliderFloat("Indirect clamp (0 = off)", &Edit.IndirectClamp, 0.0f, 100.0f, "%.1f");
	ImGui::SliderFloat("Min roughness", &Edit.MinRoughness, 0.0f, 0.5f, "%.3f");
	ImGui::Checkbox("Normal maps", &Edit.bNormalMaps);
	ImGui::Checkbox("Path regularisation (real time)", &Edit.bPathRegularization);
	ImGui::Checkbox("Portal-guided sky sampling", &Edit.bPortalGuiding);
	ImGui::Checkbox("Legacy portal pre-filter", &Edit.bPortalPreFilter);
	ImGui::Checkbox("Primary surface replacement (mirrors)", &Edit.bPrimarySurfaceReplacement);
	const bool bSerAvailable = Programs && Programs->IsSerAvailable();
	ImGui::BeginDisabled(!bSerAvailable);
	ImGui::Checkbox("Shader execution reordering", &Edit.bShaderExecutionReordering);
	ImGui::EndDisabled();
	if (!bSerAvailable)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("(unavailable)");
	}
	if (ImGui::TreeNode("Validation"))
	{
		ImGui::Checkbox("BSDF sampling only (no NEE)", &Edit.bDisableNee);
		ImGui::Checkbox("NEE only for emitters", &Edit.bDisableBsdfMis);
		ImGui::Checkbox("White furnace", &Edit.bWhiteFurnace);
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Radiance cache (real time)"))
	{
		RadianceCache.DrawSettingsUi(EditCache);
		ImGui::TreePop();
	}
	static const char* kDebugModes[] = {
		"None", "Base colour", "Shading normal", "Geometric normal", "Roughness", "Metallic", "Emission",
		"Motion vectors", "View Z", "Path length", "Direct only", "Indirect only", "UV", "Instance / section", "First bounce hit",
		"Radiance cache", "Radiance cache cells" };
	int mode = static_cast<int>(Edit.DebugMode);
	if (ImGui::Combo("Debug view", &mode, kDebugModes, static_cast<int>(std::size(kDebugModes))))
	{
		Edit.DebugMode = static_cast<uint32>(mode);
	}
}
}
