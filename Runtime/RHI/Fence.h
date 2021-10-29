#pragma once
#include "Core/RefPtr.hpp"
#include "GfxDevice/Vulkan/VulkanFence.h"
#include "Types.h"

using namespace GfxDevice::Vulkan;

namespace Sailor::RHI
{
	class Mesh;

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

		void Wait(uint64_t timeout = UINT64_MAX) const;
		void Reset() const;
		bool IsFinished() const;
	};
};
