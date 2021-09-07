#pragma once
#include "Core/RefPtr.hpp"
#include <glm/glm/glm.hpp>

namespace Sailor::RHI
{
	class RHIVertex
	{
	public:
		glm::vec2 m_position;
		glm::vec3 m_color;
	};

	class RHIResource : public TRefBase
	{
	public:

	protected:

		RHIResource() = default;
		virtual ~RHIResource() = default;

	private:

		RHIResource(RHIResource&& move) = delete;
		RHIResource(RHIResource& copy) = delete;
		RHIResource& operator =(RHIResource& rhs) = delete;
	};

	class RHICommandList : public RHIResource
	{
	public:

	//	RHICommandBuffer(Device* device, CommandPool* commandPool, VkCommandBufferLevel level);
	//	virtual ~RHICommandBuffer() override;
	};


};