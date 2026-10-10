#pragma once

#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Renderer.h"
#include <cstring>
#include <stdexcept>
#include <utility>

namespace Sailor::Tests
{
	inline RHI::RHIBufferPtr RecordBufferReadback(RHI::RHICommandListPtr command,
		const Memory::TManagedMemoryPtr<Memory::VulkanBufferMemoryPtr, RHI::RHIBuffer::VulkanBufferAllocator>& source,
		size_t size, size_t offset = 0)
	{
		using namespace RHI;
		using namespace GraphicsDriver::Vulkan;
		auto& driver = Renderer::GetDriver();
		if (size == 0 || offset > source->Get().m_size || size > source->Get().m_size - offset)
		{
			throw std::runtime_error("buffer readback must stay inside the source allocation");
		}

		auto& buffer = command->m_vulkan.m_commandBuffer;
		auto native = *source->Get();
		if (!(native.m_buffer->m_usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
		{
			// Production pools need no TRANSFER_SRC usage. Read their existing
			// allocation through a temporary transfer alias, without reuploading.
			auto memory = *native.m_buffer->GetMemoryPtr();
			auto alias = VulkanBufferPtr::Make(VulkanApi::GetInstance()->GetMainDevice(),
				native.m_buffer->m_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, native.m_buffer->m_sharingMode);
			alias->Compile();
			const auto requirements = alias->GetMemoryRequirements();
			const auto sourceRequirements = native.m_buffer->GetMemoryRequirements();
			if (memory.m_offset % requirements.alignment || requirements.size > memory.m_size ||
				(requirements.memoryTypeBits & sourceRequirements.memoryTypeBits) != sourceRequirements.memoryTypeBits)
			{
				throw std::runtime_error("buffer allocation must support a transfer readback alias");
			}
			// Bind without transferring ownership of the allocation to the alias.
			if (alias->Bind(memory.m_deviceMemory, memory.m_offset) != VK_SUCCESS)
			{
				throw std::runtime_error("readback alias must bind the uploaded device memory");
			}
			buffer->AddDependency(alias);
			native.m_buffer = std::move(alias);
		}
		auto output = driver->CreateBuffer(size, EBufferUsageBit::BufferTransferDst_Bit,
			EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
		std::memset(output->GetPointer(), 0xcd, size);
		buffer->AddDependency(source);
		buffer->AddDependency(output->m_vulkan.m_buffer);
		buffer->MemoryBarrier(VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
		buffer->CopyBuffer(native, *output->m_vulkan.m_buffer->Get(), size, offset);
		buffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
		return output;
	}

	inline RHI::RHIBufferPtr ReadBuffer_Immediate(RHI::RHIBufferPtr source, size_t size, size_t offset = 0)
	{
		auto& driver = RHI::Renderer::GetDriver();
		auto commands = RHI::Renderer::GetDriverCommands();
		auto command = driver->CreateCommandList(false, RHI::ECommandListQueue::Transfer);
		commands->BeginCommandList(command, true);
		auto output = RecordBufferReadback(command, source->m_vulkan.m_buffer, size, offset);
		commands->EndCommandList(command);
		if (!driver->SubmitCommandList_Immediate(command))
		{
			throw std::runtime_error("buffer readback must finish");
		}
		return output;
	}
}
