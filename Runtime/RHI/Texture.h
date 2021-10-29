#pragma once
#include "Core/RefPtr.hpp"
#include "GfxDevice/Vulkan/VulkanBuffer.h"
#include "Types.h"

using namespace GfxDevice::Vulkan;

namespace Sailor::RHI
{
	class Texture : public Resource
	{
	public:
#if defined(VULKAN)
		struct
		{
			TRefPtr<VulkanImage> m_image;
		} m_vulkan;
#endif

		bool IsReady() const { return true; }
	};
};
