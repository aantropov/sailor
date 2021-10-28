#pragma once
#include "Core/RefPtr.hpp"
#include "GfxDevice/Vulkan/VulkanBuffer.h"
#include "RHIResource.h"

using namespace GfxDevice::Vulkan;

namespace Sailor::RHI
{
	class Mesh : public Resource
	{
	public:
#if defined(VULKAN)
		struct
		{
			TRefPtr<VulkanBuffer> m_vertexBuffer;
			TRefPtr<VulkanBuffer> m_indexBuffer;
		} m_vulkan;
#endif

		size_t m_verticesNum;
		size_t m_indicesNum;
		
		bool IsReady() const { return true; }
	};
};
