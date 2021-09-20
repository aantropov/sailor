#pragma once
#include "Core/RefPtr.hpp"
#include "VulkanDevice.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanGraphicsPipeline : public TRefBase
	{
	public:
		VulkanGraphicsPipeline() = default;

	protected:

		virtual ~VulkanGraphicsPipeline() = default;
	};
}
