#pragma once

#include "Core.h"
#include "Containers/StringView.h"
#include "Math/Math.h"


namespace frt
{
class CWorldScene;
}

namespace frt::config
{
class CConfig;
}

namespace frt::graphics
{
class CRenderer;
class CRenderView;
}


namespace frt
{
/**
 * Launch-time configuration: the [Render], [Post], [Taa], [Denoiser], [Display] and [Scene]
 * sections, usually from the command line (-Render:Mode=Reference -Scene:TimeOfDay=0.3 ...).
 * Applied on top of the saved user settings without touching them.
 */

// [Render] / [Post] / [Taa] / [Denoiser] keys into the renderer's shared profile; [Display] VSync.
FRT_CORE_API void ApplyRenderLaunchConfig (const config::CConfig& Config, graphics::CRenderer& Renderer);

// -Render:RenderMode=Raytracing|Raster: a session-only override of the saved render mode.
FRT_CORE_API void ApplyViewLaunchConfig (const config::CConfig& Config, graphics::CRenderView& View);

// [Scene] TimeOfDay / CameraPosition / CameraRotation and -Render:FieldOfViewY into a world.
// [Scene] PauseWorld is the host's (it pauses the simulation, not a world property).
FRT_CORE_API void ApplySceneLaunchConfig (const config::CConfig& Config, CWorldScene& World);

// "x,y,z", spaces around the numbers allowed ("1, 2, 3"): the vectors of the [Scene] and
// [Automation] keys. False, with Out unchanged, for anything else. Core's own (not exported).
bool ParseVector3 (FStringView Text, Vector3f& Out);
}
