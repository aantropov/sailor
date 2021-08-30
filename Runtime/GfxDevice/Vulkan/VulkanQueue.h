#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"

using namespace Sailor;
namespace Sailor::GfxDevice::Vulkan
{
	class VulkanFence;

	class VulkanQueue final : public TRefBase
	{

	public:

		operator VkQueue() const { return m_queue; }

		uint32_t QueueFamilyIndex() const { return m_queueFamilyIndex; }
		uint32_t QueueIndex() const { return m_queueIndex; }

		VkResult Submit(const std::vector<VkSubmitInfo>& submitInfos, TRefPtr<VulkanFence> fence = nullptr);
		VkResult Submit(const VkSubmitInfo& submitInfo, TRefPtr<VulkanFence> fence = nullptr);
		VkResult Present(const VkPresentInfoKHR& info);
		VkResult WaitIdle();

	protected:

		VulkanQueue(VkQueue queue, uint32_t queueFamilyIndex, uint32_t queueIndex);
		virtual ~VulkanQueue();

		VulkanQueue() = delete;
		VulkanQueue(const VulkanQueue&) = delete;
		VulkanQueue& operator=(const VulkanQueue&) = delete;

		// allow only Device to create Queue to ensure that queues are shared
		friend class VulkanApi;
		friend class VulkanDevice;
		friend class TRefPtr<VulkanQueue>;

		VkQueue m_queue;
		uint32_t m_queueFamilyIndex;
		uint32_t m_queueIndex;
		std::mutex m_mutex;
	};
}