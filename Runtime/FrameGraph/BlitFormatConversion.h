#pragma once

#include "Core/Defines.h"
#include "RHI/Types.h"

namespace Sailor::Framegraph
{
	SAILOR_API bool RequiresShaderColorBlitForFormatConversion(
		RHI::ETextureFormat srcFormat,
		RHI::ETextureFormat dstFormat);
}
