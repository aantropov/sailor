#pragma once
#include "Memory/RefPtr.hpp"
#include "GraphicsDriver/Vulkan/VulkanBuffer.h"
#include "Texture.h"
#include "Types.h"

using namespace GraphicsDriver::Vulkan;

namespace Sailor::RHI
{
	class RHIRenderTarget : public RHITexture
	{
	public:

		SAILOR_API RHIRenderTarget(ETextureFiltration filtration, ETextureClamping clamping, bool bShouldGenerateMips, EImageLayout defaultLayout = EImageLayout::ShaderReadOnlyOptimal) :
			RHITexture(filtration, clamping, bShouldGenerateMips, defaultLayout)
		{}

	};
};
