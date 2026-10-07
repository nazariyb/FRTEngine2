#include "GameInstance.h"


namespace frt
{
// Out of line, so the base class's vtable and destructor are emitted once, in Core.
GameInstance::~GameInstance () = default;
}
