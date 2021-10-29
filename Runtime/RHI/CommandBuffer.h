#include "Core/RefPtr.hpp"
#include "GfxDevice/Vulkan/VulkanCommandBuffer.h"
#include "Types.h"

using namespace GfxDevice::Vulkan;

namespace Sailor::RHI
{
	class CommandBuffer : public Resource
	{
	public:

#if defined(VULKAN)
		struct
		{
			TRefPtr<VulkanCommandBuffer> m_commandBuffer;
		} m_vulkan;
#endif

	};
};
