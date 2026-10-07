#pragma once

#include "Core/Defines.h"

namespace Sailor::Mac
{
	SAILOR_SHARED_API void* GetNativeView(void* nativeWindow);
	SAILOR_SHARED_API void* GetMetalLayer(void* nativeWindow, bool bVsyncRequested);
}
