#pragma once
#include "Core/RefPtr.hpp"
#include "GfxDevice/Vulkan/VulkanBuffer.h"
#include "Types.h"

using namespace GfxDevice::Vulkan;

namespace Sailor::RHI
{
	class Texture : public Resource, public IDelayedInitialization
	{
	public:
#if defined(VULKAN)
		struct
		{
			VulkanImagePtr m_image;
		} m_vulkan;
#endif
	};
};
