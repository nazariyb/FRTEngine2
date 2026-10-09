#include "Graphics/Render/RenderContext.h"

#include "EnginePaths.h"


namespace frt::graphics
{
rhi::SShaderCompileDesc MakeShaderDesc (
	FStringView RelativePath,
	FStringView EntryPoint,
	FStringView Profile,
	std::initializer_list<rhi::SShaderDefine> Defines)
{
	rhi::SShaderCompileDesc desc;
	desc.SourcePath = paths::GetShaderRootDir() / RelativePath.ToPath();
	desc.EntryPoint = EntryPoint;
	desc.Profile = Profile;
	desc.Defines.assign(Defines.begin(), Defines.end());
	return desc;
}
}
