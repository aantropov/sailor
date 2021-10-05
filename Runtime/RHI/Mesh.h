#pragma once
#include "Core/RefPtr.hpp"
#include <glm/glm/glm.hpp>
#include <glm/glm/gtx/hash.hpp>
#include "GfxDevice/Vulkan/VulkanBuffer.h"
#include "RHIResource.h"

using namespace GfxDevice::Vulkan;

namespace Sailor::RHI
{
	class Mesh
	{
	public:

		void Compile(const std::vector<Vertex> vertices, const std::vector<Vertex> indices);
		
		bool IsReady() const;

	protected:

		uint8_t m_materialIndex;
		
		TRefPtr<VulkanBuffer> m_vertices;
		TRefPtr<VulkanBuffer> m_indices;

		const VkDeviceSize m_verticesBufferSize;
		const VkDeviceSize m_indicesBufferSize;
	};
};
