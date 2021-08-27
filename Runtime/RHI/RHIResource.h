#pragma once

namespace Sailor::RHI
{
	class RHIResource
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

	class RHICommandBuffer : public RHIResource
	{
	public:

	//	RHICommandBuffer(Device* device, CommandPool* commandPool, VkCommandBufferLevel level);
	//	virtual ~RHICommandBuffer() override;
	};


};