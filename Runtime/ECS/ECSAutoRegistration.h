#pragma once

#include "Core/Defines.h"

namespace Sailor::ECS
{
	SAILOR_API void SetAutoRegistrationSuppressed(bool suppressed);
	SAILOR_API bool IsAutoRegistrationSuppressed();
}
