#include "Controller.h"


namespace frt
{
	CController::CController()
	{
		Camera = memory::NewShared<graphics::CCamera>();
	}
}
