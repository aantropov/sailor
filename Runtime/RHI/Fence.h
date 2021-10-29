#pragma once
#include "Core/RefPtr.hpp"
#include "GfxDevice/Vulkan/VulkanFence.h"
#include "RHIResource.h"

using namespace GfxDevice::Vulkan;

namespace Sailor::RHI
{
	class IVisitor
	{
	public:

		virtual ~IVisitor() = default;

		void AddDependency(TRefPtr<class RHI::Mesh> mesh);
		void TraceDependencies();

	protected:

		virtual void TraceDependence() = 0;
	};

	class Fence : public Resource
	{
	public:
#if defined(VULKAN)
		struct
		{
			TRefPtr<VulkanFence> m_fence;
		} m_vulkan;
#endif
		void Wait(uint64_t timeout = UINT64_MAX) const
		{
#if defined(VULKAN)
			m_vulkan.m_fence->Wait(timeout);
#endif
		}

		void Reset() const
		{
#if defined(VULKAN)
			m_vulkan.m_fence->Reset();
#endif
		}

		bool IsFinished() const
		{
#if defined(VULKAN)
			return m_vulkan.m_fence->Status() == VkResult::VK_SUCCESS;
#endif
			return true;
		}
	};
};
