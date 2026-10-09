#pragma once

#include "Graphics/Render/RenderContext.h"


namespace frt::graphics
{
/**
 * The path tracer's GPU programs, shared by every CPathTracer (one per render view): the ray
 * tracing state objects with their shader tables - a Shader Execution Reordering build on
 * SM 6.9 devices and a plain SM 6.8 build - and the remodulation pipeline.
 *
 * State objects do not go through CPipelineLibrary (it only compiles the DXIL libraries they
 * are made from), so without this every view would create its own: a driver compile each.
 * Update() rebuilds a state object when its library was hot-reloaded; the renderer calls it
 * once per frame before any view traces.
 */
class FRT_CORE_API CPathTracerPrograms
{
public:
	CPathTracerPrograms () = default;
	CPathTracerPrograms (const CPathTracerPrograms&) = delete;
	CPathTracerPrograms& operator= (const CPathTracerPrograms&) = delete;

	void Initialize (rhi::CDevice& Device, rhi::CPipelineLibrary& Pipelines);
	void Shutdown ();

	void Update (ID3D12GraphicsCommandList* CommandList);

	struct SVariant
	{
		rhi::HShaderLibrary Library = rhi::InvalidPipelineHandle;
		uint32 BuiltVersion = 0u;
		rhi::ComPtr<ID3D12StateObject> StateObject;
		rhi::SGpuBuffer ShaderTable;
		D3D12_DISPATCH_RAYS_DESC DispatchTemplate = {};
		bool bValid = false;
		bool bReorder = false;           // the SER build
	};

	bool IsReady () const { return Base.bValid || Ser.bValid; }
	bool IsSerAvailable () const { return Ser.bValid; }
	// The variant to trace with: the SER build when wanted and built, else the plain one.
	const SVariant* Select (bool bWantSer) const;
	rhi::HPipeline GetRemodulatePipeline () const { return Remodulate; }

private:
	bool Build (SVariant& Variant, FStringView Name, ID3D12GraphicsCommandList* CommandList);

	rhi::CDevice* Device = nullptr;
	rhi::CPipelineLibrary* Pipelines = nullptr;
	SVariant Ser;
	SVariant Base;
	rhi::HPipeline Remodulate = rhi::InvalidPipelineHandle;
};
}
