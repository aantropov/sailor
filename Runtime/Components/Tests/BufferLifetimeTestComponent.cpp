#include "Components/Tests/BufferLifetimeTestComponent.h"
#include "Platform/Time.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Material.h"
#include "RHI/Mesh.h"
#include "RHI/Renderer.h"
#include "RHI/RenderTarget.h"
#include "RHI/VertexDescription.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	using Allocation = Memory::TManagedMemory<Memory::VulkanBufferMemoryPtr, VulkanBufferAllocator>;
	using Values = std::array<uint32_t, 4>;
	constexpr Values ValueA{ 17u, 31u, 0x12345678u, 0xfedcba98u };
	constexpr Values ValueB{ 53u, 79u, 0x87654321u, 0xabcddcbau };
	constexpr Values Neighbor{ 101u, 307u, 0xc001d00du, 0x5a6b7c8du };
	constexpr uint32_t Side = 8u;
	const EMemoryPropertyFlags HostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
	const EBufferUsageFlags TransferUsage = EBufferUsageBit::BufferTransferSrc_Bit | EBufferUsageBit::BufferTransferDst_Bit;

	TSharedPtr<VulkanBufferAllocator> CreatePool()
	{
		auto pool = TSharedPtr<VulkanBufferAllocator>::Make(4096u, 64u, 4096u);
		pool->GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		pool->GetGlobalAllocator().SetUsage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
		return pool;
	}

	RHIBufferPtr AllocateBuffer(TSharedPtr<VulkanBufferAllocator> pool, size_t size,
		EBufferUsageFlags usage = TransferUsage, size_t alignment = 16u)
	{
		auto buffer = RHIBufferPtr::Make(usage, HostMemory);
		buffer->m_vulkan.m_buffer = TSharedPtr<Allocation>::Make(pool->Allocate(size, alignment), pool);
		return buffer;
	}

	std::string Submit(RHICommandListPtr cmd)
	{
		auto fence = RHIFencePtr::Make();
		if (!Renderer::GetDriver()->SubmitCommandList(cmd, fence)) return "submission failed";
		fence->Wait(5000000000ull);
		if (!fence->IsFinished()) return "fence timed out; pending command dependencies retained";
		fence->ClearDependencies();
		return {};
	}

	std::string ValidateSharedOwner()
	{
		auto& driver = Renderer::GetDriver();
		auto buffer = driver->CreateBuffer(sizeof(Values), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
		auto first = driver->CreateShaderBindings();
		auto second = driver->CreateShaderBindings();
		auto a = driver->AddBufferToShaderBindings(first, buffer, "source"_h, 0u);
		auto b = driver->AddBufferToShaderBindings(second, buffer, "source"_h, 0u);
		if (!a || !b) return "shared buffer binding setup failed";
		if (a->m_vulkan.m_valueBinding != b->m_vulkan.m_valueBinding)
			return "bindings did not share the original buffer allocation";
		return {};
	}

	std::string ValidateUploads()
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto pool = CreatePool();
		auto prefix = AllocateBuffer(pool, 13u, TransferUsage, 1u);
		auto buffer = AllocateBuffer(pool, 2u * sizeof(Values));
		const auto range = buffer->m_vulkan.m_buffer->Get();
		if (!range.m_alignmentOffset || buffer->GetOffset() != (*range).m_offset || buffer->GetSize() != 2u * sizeof(Values))
			return "pooled buffer lost its padded offset or size";
		std::memset(buffer->GetPointer(), 0, buffer->GetSize());
		TWeakPtr<Allocation> owner(buffer->m_vulkan.m_buffer);
		std::array<RHICommandListPtr, 2> uploads;
		std::array<RHIBufferPtr, 2> readbacks;
		for (size_t i = 0u; i < uploads.size(); ++i)
		{
			auto& cmd = uploads[i];
			cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(cmd, true);
			cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
			Values source = ValueA;
			commands->UpdateBuffer(cmd, buffer, source.data(), sizeof(source), 8u);
			source.fill(0xdeadbeefu);
			readbacks[i] = driver->CreateBuffer(buffer->GetSize(), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
			cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			// A raw copy retains only the backing buffer, so it cannot hide missing upload ownership.
			cmd->m_vulkan.m_commandBuffer->CopyBuffer(*range, *readbacks[i]->m_vulkan.m_buffer->Get(), buffer->GetSize());
			cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
			commands->EndCommandList(cmd);
		}
		buffer.Clear();
		auto neighbor = AllocateBuffer(pool, 2u * sizeof(Values));
		if (!owner.TryLock() || neighbor->m_vulkan.m_buffer->Get() == range)
			return "an unsubmitted upload released its pooled destination";
		for (size_t i = 0u; i < uploads.size(); ++i)
		{
			if (auto error = Submit(uploads[i]); !error.empty()) return error;
			const auto* actual = static_cast<const uint32_t*>(readbacks[i]->GetPointer());
			for (uint32_t word = 0u; word < 8u; ++word)
				if (actual[word] != (word >= 2u && word < 6u ? ValueA[word - 2u] : 0u))
					return "pooled partial upload corrupted its prefix, payload or suffix";
			uploads[i]->m_vulkan.m_commandBuffer->Reset();
			if (static_cast<bool>(owner.TryLock()) != (i == 0u))
				return "upload allocation did not follow the last recorded command";
		}
		auto reused = AllocateBuffer(pool, 2u * sizeof(Values));
		if (reused->m_vulkan.m_buffer->Get() != range) return "completed upload range was not reusable";
		return {};
	}

	std::string ValidateImageCopies()
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto pool = CreatePool();
		auto prefix = AllocateBuffer(pool, 13u, TransferUsage, 1u);
		auto source = AllocateBuffer(pool, sizeof(Values));
		auto destination = AllocateBuffer(pool, sizeof(Values));
		std::memcpy(source->GetPointer(), ValueA.data(), sizeof(ValueA));
		std::memset(destination->GetPointer(), 0, sizeof(Values));
		auto* result = static_cast<const uint32_t*>(destination->GetPointer());
		TWeakPtr<Allocation> sourceOwner(source->m_vulkan.m_buffer), destinationOwner(destination->m_vulkan.m_buffer);
		auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(cmd, true);
		auto image = driver->CreateRenderTarget(cmd, glm::ivec2(1), 1u, EFormat::R32G32B32A32_UINT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp,
			ETextureUsageBit::ColorAttachment_Bit | ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit);
		cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
		commands->ImageMemoryBarrier(cmd, image, EImageLayout::TransferDstOptimal);
		commands->CopyBufferToImage(cmd, source, image);
		commands->ImageMemoryBarrier(cmd, image, EImageLayout::TransferSrcOptimal);
		commands->CopyImageToBuffer(cmd, image, destination);
		cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
		commands->EndCommandList(cmd);
		source.Clear();
		destination.Clear();
		if (!sourceOwner.TryLock() || !destinationOwner.TryLock()) return "image copy lost a pooled buffer before submission";
		if (auto error = Submit(cmd); !error.empty()) return error;
		if (!std::equal(ValueA.begin(), ValueA.end(), result)) return "pooled image round trip changed the bytes";
		cmd->m_vulkan.m_commandBuffer->Reset();
		if (sourceOwner.TryLock() || destinationOwner.TryLock()) return "image copy retained a buffer after command reset";
		return {};
	}

	std::string ValidateDescriptors(ShaderSetPtr shader, bool projected)
	{
		auto& driver = Renderer::GetDriver();
		auto* nativeDriver = driver.DynamicCast<VulkanGraphicsDriver>();
		auto commands = Renderer::GetDriverCommands();
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		const size_t alignment = (std::max)(size_t(16u), size_t(device->GetMinSsboOffsetAlignment()));
		auto pool = CreatePool();
		auto prefix = AllocateBuffer(pool, 13u, TransferUsage, 1u);
		auto a = AllocateBuffer(pool, sizeof(Values), EBufferUsageBit::StorageBuffer_Bit, alignment);
		auto b = AllocateBuffer(pool, sizeof(Values), EBufferUsageBit::StorageBuffer_Bit, alignment);
		const auto range = a->m_vulkan.m_buffer->Get();
		TWeakPtr<Allocation> owner(a->m_vulkan.m_buffer);
		std::memcpy(a->GetPointer(), ValueA.data(), sizeof(ValueA));
		std::memcpy(b->GetPointer(), ValueB.data(), sizeof(ValueB));
		auto neighbor = driver->CreateBuffer(sizeof(Values), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
		std::memcpy(neighbor->GetPointer(), Neighbor.data(), sizeof(Neighbor));
		auto inputs = driver->CreateShaderBindings();
		if (!driver->AddBufferToShaderBindings(inputs, neighbor, "neighbor"_h, 0u) ||
			(projected && !driver->AddBufferToShaderBindings(inputs, neighbor, "unused"_h, 31u)))
			return "pooled descriptor input setup failed";
		auto binding = driver->AddBufferToShaderBindings(inputs, a, "source"_h, 1u);
		if (!binding || binding->m_vulkan.m_valueBinding != a->m_vulkan.m_buffer)
			return "binding did not retain the original pooled allocation";
		{
			const auto original = inputs->m_vulkan.m_descriptorSet;
			const auto revision = inputs->GetDescriptorRevision();
			auto unavailable = RHIBufferPtr::Make(EBufferUsageBit::StorageBuffer_Bit, HostMemory);
			auto uncompiled = VulkanBufferPtr::Make(device, sizeof(Values), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE);
			unavailable->m_vulkan.m_buffer = TSharedPtr<Allocation>::Make(
				TMemoryPtr<VulkanBufferMemoryPtr>(0u, 0u, sizeof(Values), VulkanBufferMemoryPtr(uncompiled, 0u, sizeof(Values)), UINT32_MAX),
				TWeakPtr<VulkanBufferAllocator>{});
			if (driver->AddBufferToShaderBindings(inputs, unavailable, "source"_h, 1u) ||
				inputs->m_vulkan.m_descriptorSet != original || inputs->GetDescriptorRevision() != revision ||
				binding->m_vulkan.m_valueBinding != a->m_vulkan.m_buffer)
				return "rejected replacement changed the published pooled owner";
		}
		auto pipeline = nativeDriver->GetOrAddComputePipeline(shader->GetComputeShaderRHI());
		auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(cmd, true);
		cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		std::array<RHIBufferPtr, 2> outputs;
		for (uint32_t i = 0u; i < outputs.size(); ++i)
		{
			if (i == 1u && driver->AddBufferToShaderBindings(inputs, b, "source"_h, 1u) != binding)
				return "replacing the pooled source changed binding identity";
			if (nativeDriver->IsCompatible(pipeline->m_layout, { inputs })[0] == projected)
				return "pooled source did not use the requested direct/projected path";
			outputs[i] = driver->CreateBuffer(2u * sizeof(Values), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
			std::memset(outputs[i]->GetPointer(), 0xa7, outputs[i]->GetSize());
			auto output = driver->CreateShaderBindings();
			if (!driver->AddBufferToShaderBindings(output, outputs[i], "outputValue"_h, 0u)) return "compute output binding failed";
			commands->Dispatch(cmd, shader->GetComputeShaderRHI(), 1u, 1u, 1u, { inputs, output });
		}
		cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
		commands->EndCommandList(cmd);
		a.Clear();
		b.Clear();
		binding.Clear();
		inputs.Clear();
		nativeDriver->CollectGarbage_RenderThread();
		auto competing = AllocateBuffer(pool, sizeof(Values), EBufferUsageBit::StorageBuffer_Bit, alignment);
		if (!owner.TryLock() || competing->m_vulkan.m_buffer->Get() == range)
			return "recorded dispatch released the previous pooled source";
		if (auto error = Submit(cmd); !error.empty()) return error;
		for (size_t i = 0u; i < outputs.size(); ++i)
		{
			const auto* actual = static_cast<const uint32_t*>(outputs[i]->GetPointer());
			const auto& expected = i == 0u ? ValueA : ValueB;
			if (!std::equal(expected.begin(), expected.end(), actual) || !std::equal(Neighbor.begin(), Neighbor.end(), actual + 4u))
				return "recorded dispatch read the wrong pooled source or neighbor";
		}
		cmd->m_vulkan.m_commandBuffer->Reset();
		nativeDriver->CollectGarbage_RenderThread();
		if (owner.TryLock()) return "pooled descriptor allocation survived its final command";
		auto reused = AllocateBuffer(pool, sizeof(Values), EBufferUsageBit::StorageBuffer_Bit, alignment);
		if (reused->m_vulkan.m_buffer->Get() != range) return "completed descriptor range was not reusable";
		return {};
	}

	void UpdateQuad(RHIMeshPtr mesh, float left, float right)
	{
		const glm::vec2 vertices[] = { {left, -1}, {right, -1}, {right, 1}, {left, 1} };
		const uint32_t indices[] = { 0u, 1u, 2u, 0u, 2u, 3u };
		Renderer::GetDriver()->UpdateMesh(mesh, vertices, sizeof(vertices), indices, sizeof(indices));
	}
}

BufferLifetimeTestComponent::~BufferLifetimeTestComponent()
{
	if (m_validation) m_validation->Wait();
}

std::string BufferLifetimeTestComponent::Prepare()
{
	// Factory-only gate precedes all client-side RHIBuffer layout access on older libraries.
	if (auto error = ValidateSharedOwner(); !error.empty()) return error;
	if (auto error = ValidateUploads(); !error.empty()) return error;
	if (auto error = ValidateImageCopies(); !error.empty()) return error;
	for (bool projected : { false, true })
		if (auto error = ValidateDescriptors(m_shaders[1], projected); !error.empty()) return error;
	m_mesh = Renderer::GetDriver()->CreateMesh();
	m_mesh->m_vertexDescription = RHIVertexDescriptionPtr::Make();
	m_mesh->m_vertexDescription->SetVertexStride(sizeof(glm::vec2));
	m_mesh->m_vertexDescription->AddAttribute(0u, 0u, EFormat::R32G32_SFLOAT, 0u);
	UpdateQuad(m_mesh, -1.0f, 0.0f);
	return {};
}

std::string BufferLifetimeTestComponent::RecordAndReplace()
{
	auto& driver = Renderer::GetDriver();
	auto commands = Renderer::GetDriverCommands();
	const RenderState state(false, false, 0.0f, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0u, false);
	auto material = driver->CreateMaterial(m_mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, m_shaders[0]);
	if (!material) return "mesh lifetime material could not be created";
	m_vertices = m_mesh->m_vertexBuffer->m_vulkan.m_buffer;
	m_indices = m_mesh->m_indexBuffer->m_vulkan.m_buffer;
	m_indirectPool = CreatePool();
	auto prefix = AllocateBuffer(m_indirectPool, 13u, TransferUsage, 1u);
	for (size_t i = 0u; i < m_draws.size(); ++i)
	{
		auto& cmd = m_draws[i];
		cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(cmd, true);
		auto color = driver->CreateRenderTarget(cmd, glm::ivec2(Side), 1u, EFormat::R32G32B32A32_SFLOAT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp,
			ETextureUsageBit::ColorAttachment_Bit | ETextureUsageBit::TextureTransferSrc_Bit);
		auto depth = driver->CreateRenderTarget(cmd, glm::ivec2(Side), 1u, EFormat::D32_SFLOAT_S8_UINT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit);
		commands->ImageMemoryBarrier(cmd, color, EImageLayout::ColorAttachmentOptimal);
		commands->ImageMemoryBarrier(cmd, depth, EImageLayout::DepthStencilAttachmentOptimal);
		RHIBufferPtr indirect;
		if (i == 1u)
		{
			indirect = AllocateBuffer(m_indirectPool, 32u, EBufferUsageBit::IndirectBuffer_Bit);
			std::memset(indirect->GetPointer(), 0xa7, indirect->GetSize());
			const VkDrawIndexedIndirectCommand draw{ m_mesh->GetIndexCount(), 1u, m_mesh->GetFirstIndex(),
				static_cast<int32_t>(m_mesh->GetVertexOffset()), 0u };
			commands->UpdateBuffer(cmd, indirect, &draw, sizeof(draw), 8u);
			const auto* bytes = static_cast<const uint8_t*>(indirect->GetPointer());
			if (std::memcmp(bytes + 8u, &draw, sizeof(draw)) != 0 || bytes[0] != 0xa7 || bytes[31] != 0xa7)
				return "indirect host upload ignored its pooled range or byte offset";
			m_indirect = indirect->m_vulkan.m_buffer;
		}
		cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
			VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
		commands->BeginRenderPass(cmd, TVector<RHITexturePtr>{ color }, depth, glm::ivec4(0, 0, Side, Side),
			glm::ivec2(0), true, glm::vec4(0, 0, 0, 1), 1.0f, false, false);
		commands->BindMaterial(cmd, material);
		commands->SetViewport(cmd, 0, 0, Side, Side, glm::vec2(0), glm::vec2(Side), 0.0f, 1.0f);
		commands->BindVertexBuffer(cmd, m_mesh->m_vertexBuffer, 0u);
		commands->BindIndexBuffer(cmd, m_mesh->m_indexBuffer, 0u, false);
		if (indirect) commands->DrawIndexedIndirect(cmd, indirect, 8u, 1u, sizeof(VkDrawIndexedIndirectCommand));
		else commands->DrawIndexed(cmd, m_mesh->GetIndexCount(), 1u, m_mesh->GetFirstIndex(), m_mesh->GetVertexOffset(), 0u);
		commands->EndRenderPass(cmd);
		m_readbacks[i] = driver->CreateBuffer(Side * Side * sizeof(glm::vec4), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
		commands->ImageMemoryBarrier(cmd, color, EImageLayout::TransferSrcOptimal);
		commands->CopyImageToBuffer(cmd, color, m_readbacks[i]);
		cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
		commands->EndCommandList(cmd);
	}
	UpdateQuad(m_mesh, 0.0f, 1.0f);
	return {};
}

std::string BufferLifetimeTestComponent::CheckDraws()
{
	if (!m_vertices.TryLock() || !m_indices.TryLock() || !m_indirect.TryLock())
		return "mesh replacement released an allocation used by recorded draws";
	if (m_vertices.TryLock() == m_mesh->m_vertexBuffer->m_vulkan.m_buffer ||
		m_indices.TryLock() == m_mesh->m_indexBuffer->m_vulkan.m_buffer)
		return "UpdateMesh did not replace the original vertex/index allocations";
	for (size_t i = 0u; i < m_draws.size(); ++i)
	{
		if (auto error = Submit(m_draws[i]); !error.empty()) return error;
		const auto* pixels = static_cast<const glm::vec4*>(m_readbacks[i]->GetPointer());
		for (uint32_t pixel = 0u; pixel < Side * Side; ++pixel)
		{
			const glm::vec4 expected = pixel % Side < Side / 2u ? glm::vec4(0.25f, 0.75f, 0.125f, 1) : glm::vec4(0, 0, 0, 1);
			for (uint32_t channel = 0u; channel < 4u; ++channel)
				if (!std::isfinite(pixels[pixel][channel]) || std::abs(pixels[pixel][channel] - expected[channel]) > 0.00001f)
					return std::format("retained mesh draw {} pixel {} channel {}: expected {}, got {}",
						i, pixel, channel, expected[channel], pixels[pixel][channel]);
		}
		m_draws[i]->m_vulkan.m_commandBuffer->Reset();
		if (static_cast<bool>(m_vertices.TryLock()) != (i == 0u) || static_cast<bool>(m_indices.TryLock()) != (i == 0u))
			return "mesh allocation did not follow the last recorded draw";
	}
	if (m_indirect.TryLock()) return "indirect allocation survived command reset";
	return {};
}

void BufferLifetimeTestComponent::Tick(float)
{
	if (IsFinished()) return;
	if (m_validation)
	{
		if (!m_validation->IsFinished()) return;
		const std::string error = m_validation->GetResult();
		m_validation.Clear();
		if (!error.empty()) { MarkFailed(error); return; }
	}
	if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 30000)
	{ MarkFailed("buffer lifetime setup or mesh upload exceeded 30 seconds"); return; }
	if (m_phase == Phase::Start)
	{
		bool ready = true;
		for (size_t i = 0u; i < m_shaders.size(); ++i)
		{
			if (!m_shaders[i])
			{
				const char* filename = i == 0u ? "Tests/Shaders/GraphicsBindingResult.shader" : "Tests/Shaders/DescriptorPublication.shader";
				if (auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(filename))
					App::GetSubmodule<ShaderCompiler>()->LoadShader(info->GetFileId(), m_shaders[i],
						i == 0u ? TVector<std::string>{ "NO_DESCRIPTORS" } : TVector<std::string>{});
			}
			ready &= m_shaders[i] && m_shaders[i]->IsReady();
		}
		if (!ready) return;
		m_validation = Tasks::CreateTaskWithResult<std::string>("Prepare buffer lifetime test"_h, [this]() { return Prepare(); }, EThreadType::Render);
		m_phase = Phase::InitialUpload;
	}
	else if (m_phase == Phase::InitialUpload)
	{
		if (!m_mesh->IsReady()) return;
		m_validation = Tasks::CreateTaskWithResult<std::string>("Record and replace mesh"_h, [this]() { return RecordAndReplace(); }, EThreadType::Render);
		m_phase = Phase::ReplacementUpload;
	}
	else if (m_phase == Phase::ReplacementUpload)
	{
		if (!m_mesh->IsReady()) return;
		m_validation = Tasks::CreateTaskWithResult<std::string>("Submit retained mesh draws"_h, [this]() { return CheckDraws(); }, EThreadType::Render);
		m_phase = Phase::Finished;
	}
	else
	{
		AddJournalEvent("BufferLifetimeEvidence",
			"Shared original allocation; padded partial uploads and pool reuse; buffer/image round trip; direct/projected A/B compute readbacks; real UpdateMesh replacement before direct/indirect draws with full 8x8 readbacks and final-owner release");
		MarkPassed();
		return;
	}
	m_validation->Run();
}
