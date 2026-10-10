#pragma once

#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Renderer.h"
#include <cstring>
#include <stdexcept>

namespace Sailor::Tests
{
	inline RHI::RHIBufferPtr ReadMeshBuffer(RHI::RHIBufferPtr source, size_t size, size_t offset = 0)
	{
		using namespace RHI;
		using namespace GraphicsDriver::Vulkan;
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		if (size == 0 || offset > source->GetSize() || size > source->GetSize() - offset)
		{
			throw std::runtime_error("mesh readback must stay inside the source allocation");
		}

		// Mesh buffers need no TRANSFER_SRC usage at runtime. Read their existing
		// device allocation through a temporary transfer alias, without reuploading.
		auto native = *source->m_vulkan.m_buffer->Get();
		auto memory = *native.m_buffer->GetMemoryPtr();
		auto alias = VulkanBufferPtr::Make(VulkanApi::GetInstance()->GetMainDevice(),
			native.m_buffer->m_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, native.m_buffer->m_sharingMode);
		alias->Compile();
		const auto requirements = alias->GetMemoryRequirements();
		const auto sourceRequirements = native.m_buffer->GetMemoryRequirements();
		if (memory.m_offset % requirements.alignment || requirements.size > memory.m_size ||
			(requirements.memoryTypeBits & sourceRequirements.memoryTypeBits) != sourceRequirements.memoryTypeBits)
		{
			throw std::runtime_error("mesh allocation must support a transfer readback alias");
		}
		// Bind without transferring ownership of the allocation to the alias.
		if (alias->Bind(memory.m_deviceMemory, memory.m_offset) != VK_SUCCESS)
		{
			throw std::runtime_error("mesh readback alias must bind the uploaded device memory");
		}
		auto output = driver->CreateBuffer(size, EBufferUsageBit::BufferTransferDst_Bit,
			EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
		std::memset(output->GetPointer(), 0xcd, size);
		auto command = driver->CreateCommandList(false, ECommandListQueue::Transfer);
		commands->BeginCommandList(command, true);
		auto& buffer = command->m_vulkan.m_commandBuffer;
		buffer->AddDependency(source->m_vulkan.m_buffer);
		buffer->AddDependency(output->m_vulkan.m_buffer);
		buffer->AddDependency(alias);
		buffer->MemoryBarrier(VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
		buffer->CopyBuffer(alias->GetBufferMemoryPtr(), *output->m_vulkan.m_buffer->Get(), size, native.m_offset + offset);
		buffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
		commands->EndCommandList(command);
		if (!driver->SubmitCommandList_Immediate(command))
		{
			throw std::runtime_error("mesh readback must finish");
		}
		return output;
	}
}
