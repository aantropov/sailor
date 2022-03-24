#pragma once
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "RHI/Types.h"

using namespace Sailor;
namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanFence;

	class VulkanQueue final : public RHI::Resource
	{

	public:

		SAILOR_API operator VkQueue() const { return m_queue; }

		SAILOR_API uint32_t QueueFamilyIndex() const { return m_queueFamilyIndex; }
		SAILOR_API uint32_t QueueIndex() const { return m_queueIndex; }

		SAILOR_API VkResult Submit(const TVector<VkSubmitInfo>& submitInfos, VulkanFencePtr fence = nullptr) const;
		SAILOR_API VkResult Submit(const VkSubmitInfo& submitInfo, VulkanFencePtr fence = nullptr) const;
		SAILOR_API VkResult Present(const VkPresentInfoKHR& info);
		SAILOR_API VkResult WaitIdle();

		SAILOR_API VulkanQueue(VkQueue queue, uint32_t queueFamilyIndex, uint32_t queueIndex);

	protected:

		SAILOR_API virtual ~VulkanQueue();

		VulkanQueue() = delete;
		VulkanQueue(const VulkanQueue&) = delete;
		VulkanQueue& operator=(const VulkanQueue&) = delete;

		VkQueue m_queue;
		uint32_t m_queueFamilyIndex;
		uint32_t m_queueIndex;
		
		mutable SpinLock m_lock;
	};
}