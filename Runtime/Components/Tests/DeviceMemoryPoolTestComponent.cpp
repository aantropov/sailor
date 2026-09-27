#include "Components/Tests/DeviceMemoryPoolTestComponent.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanBuffer.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Renderer.h"
#include "RHI/RenderTarget.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <format>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	constexpr VkExtent3D Extent{ 8u, 8u, 1u };
	constexpr VkFormat Format = VK_FORMAT_R8G8B8A8_UNORM;
	constexpr VkImageUsageFlags ImageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	constexpr size_t ByteCount = Extent.width * Extent.height * 4u;

	struct Resource
	{
		const char* m_name;
		bool m_isBuffer;
		bool m_uploaded;
		VkImageTiling m_tiling;
		VulkanBufferPtr m_buffer{};
		VulkanImagePtr m_image{};
		std::array<uint8_t, ByteCount> m_expected{};
	};

	std::string ValidateMappedMemoryCopy(std::string& evidence)
	{
		constexpr size_t Prefix = 37u;
		constexpr size_t PayloadSize = 4u * 1024u * 1024u + 3u;
		constexpr size_t Suffix = 53u;
		constexpr size_t TotalSize = Prefix + PayloadSize + Suffix;
		const EMemoryPropertyFlags hostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
		auto buffer = Renderer::GetDriver()->CreateBuffer(TotalSize, EBufferUsageBit::BufferTransferSrc_Bit, hostMemory);
		auto range = **buffer->m_vulkan.m_buffer;
		auto* mapped = static_cast<uint8_t*>(buffer->GetPointer());
		if (!mapped) return "mapped copy validation received unmapped host-visible memory";
		TVector<uint8_t> source(PayloadSize);
		const auto pattern = [](size_t byte, uint32_t pass)
		{
			return uint8_t(byte * 29u + (byte >> 8u) + pass * 101u + 17u);
		};
		for (uint32_t pass = 0u; pass < 2u; ++pass)
		{
			const uint8_t canary = uint8_t(0xb7u + pass);
			std::memset(mapped, canary, TotalSize);
			for (size_t byte = 0u; byte < PayloadSize; ++byte) source[byte] = pattern(byte, pass);
			range.m_deviceMemory->Copy(range.m_offset + Prefix, PayloadSize, source.GetData());
			std::fill(source.begin(), source.end(), uint8_t(0xeeu));
			for (size_t byte = 0u; byte < TotalSize; ++byte)
			{
				const uint8_t expected = byte >= Prefix && byte < Prefix + PayloadSize ? pattern(byte - Prefix, pass) : canary;
				if (mapped[byte] != expected)
					return std::format("mapped copy pass {} byte {}: expected {:#x}, got {:#x}", pass, byte, expected, mapped[byte]);
			}
		}
		evidence += std::format("mapped Copy passed 2 full {}-byte payloads at buffer offset 37 (physical base {}), "
			"37/53-byte canaries and poisoned CPU sources; no GPU transfer used; ", PayloadSize, range.m_offset);
		return {};
	}

	std::string ValidatePoolSelection(VulkanBufferPtr buffer, std::string& evidence)
	{
		auto device = buffer->GetMemoryDevice()->GetDevice();
		const auto requirements = buffer->GetMemoryRequirements();
		auto* linear = &device->GetMemoryAllocator(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, requirements, EVulkanMemoryClass::Linear);
		auto* optimal = &device->GetMemoryAllocator(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, requirements, EVulkanMemoryClass::OptimalImage);
		if (linear == optimal) return "equal properties/type bits selected the same pool for both memory classes";
		for (uint32_t variant = 0u; variant < 3u; ++variant)
		{
			auto requested = requirements;
			if (variant == 1u) requested.size += requirements.alignment;
			if (variant == 2u) requested.alignment *= 2u;
			if (linear != &device->GetMemoryAllocator(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, requested, EVulkanMemoryClass::Linear) ||
				optimal != &device->GetMemoryAllocator(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, requested, EVulkanMemoryClass::OptimalImage))
				return "repeat lookup or size/alignment-only changes selected a different pool";
		}
		// No Allocate call: these key checks do not create additional backing memory.
		auto* unrestricted = &device->GetMemoryAllocator(0u, requirements, EVulkanMemoryClass::Linear);
		if (unrestricted == linear || unrestricted != &device->GetMemoryAllocator(0u, requirements, EVulkanMemoryClass::Linear))
			return "memory property requirements are not a stable part of pool identity";
		auto narrowed = requirements;
		narrowed.memoryTypeBits &= 0u - narrowed.memoryTypeBits;
		if (narrowed.memoryTypeBits != requirements.memoryTypeBits)
		{
			auto* narrowPool = &device->GetMemoryAllocator(0u, narrowed, EVulkanMemoryClass::Linear);
			if (narrowPool == unrestricted || narrowPool != &device->GetMemoryAllocator(0u, narrowed, EVulkanMemoryClass::Linear))
				return "memory type bits are not a stable part of pool identity";
			evidence += std::format("selector mask identity passed ({:#x} vs {:#x}); ", requirements.memoryTypeBits, narrowed.memoryTypeBits);
		}
		else evidence += "selector narrower-mask case SKIPPED: only one legal buffer type bit; ";
		evidence += "selector class/property identity, repeated lookup and size/alignment invariance passed without test allocations; ";
		return {};
	}

	std::string ValidateMemoryPools(std::string& evidence, VulkanBufferPtr& retainedBuffer)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(device->GetPhysicalDevice(), &properties);
		VkFormatProperties formatProperties{};
		vkGetPhysicalDeviceFormatProperties(device->GetPhysicalDevice(), Format, &formatProperties);
		evidence = std::format("bufferImageGranularity={}; RGBA8 features linear={:#x}, optimal={:#x}; ",
			properties.limits.bufferImageGranularity, formatProperties.linearTilingFeatures, formatProperties.optimalTilingFeatures);
		const auto supportsImages = [&](VkImageTiling tiling)
		{
			VkImageFormatProperties imageProperties{};
			const VkResult result = vkGetPhysicalDeviceImageFormatProperties(device->GetPhysicalDevice(), Format,
				VK_IMAGE_TYPE_2D, tiling, ImageUsage, 0u, &imageProperties);
			const bool supported = result == VK_SUCCESS && imageProperties.maxExtent.width >= Extent.width &&
				imageProperties.maxExtent.height >= Extent.height && imageProperties.maxExtent.depth >= Extent.depth &&
				imageProperties.maxMipLevels >= 1u && imageProperties.maxArrayLayers >= 1u &&
				(imageProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0u;
			evidence += std::format("{} transfer image query={}, supported={}; ",
				tiling == VK_IMAGE_TILING_LINEAR ? "linear" : "optimal", int32_t(result), supported);
			return supported;
		};
		if (!supportsImages(VK_IMAGE_TILING_OPTIMAL)) return "required optimal RGBA8 transfer images are unsupported";
		const bool hasLinearImages = supportsImages(VK_IMAGE_TILING_LINEAR);
		if (!hasLinearImages) evidence += "linear image factories SKIPPED: unsupported format/usage/extent; ";

		std::array<Resource, 6> resources{
			Resource{ "retained buffer", true, true, VK_IMAGE_TILING_LINEAR },
			Resource{ "uploaded optimal image", false, true, VK_IMAGE_TILING_OPTIMAL },
			Resource{ "uploaded linear image", false, true, VK_IMAGE_TILING_LINEAR },
			Resource{ "recycled buffer", true, true, VK_IMAGE_TILING_LINEAR },
			Resource{ "cleared optimal image", false, false, VK_IMAGE_TILING_OPTIMAL },
			Resource{ "cleared linear image", false, false, VK_IMAGE_TILING_LINEAR }
		};
		const EMemoryPropertyFlags hostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
		std::string policyFailure;
		uint32_t checkedReadbacks = 0u;
		for (uint32_t round = 0u; round < 3u; ++round)
		{
			auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(cmd, true);
			auto& native = cmd->m_vulkan.m_commandBuffer;
			std::array<VkDeviceMemory, resources.size()> memories{};
			std::array<VkMemoryRequirements, resources.size()> requirements{};
			std::array<VkDeviceSize, resources.size()> bufferOffsets{};
			std::array<RHIBufferPtr, resources.size()> readbacks{};
			std::string allocationFailure;
			for (size_t i = 0u; i < resources.size(); ++i)
			{
				auto& resource = resources[i];
				if (!resource.m_isBuffer && resource.m_tiling == VK_IMAGE_TILING_LINEAR && !hasLinearImages) continue;
				if (!resource.m_buffer && !resource.m_image)
				{
					for (size_t byte = 0u; byte < ByteCount; ++byte)
					{
						resource.m_expected[byte] = resource.m_uploaded ?
							uint8_t(19u + i * 41u + round * 67u + byte * 13u + (byte / 4u) * 7u) :
							uint8_t((byte % 4u == 3u || (byte + i + round) % 2u != 0u) ? 255u : 0u);
					}
					if (resource.m_isBuffer)
					{
						resource.m_buffer = VulkanApi::CreateBuffer(native, device, resource.m_expected.data(), ByteCount,
							VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE);
					}
					else if (resource.m_uploaded)
					{
						resource.m_image = VulkanApi::CreateImageUpload(native, device, resource.m_expected.data(), ByteCount,
							Extent, 1u, VK_IMAGE_TYPE_2D, Format, resource.m_tiling, ImageUsage,
							VK_SHARING_MODE_EXCLUSIVE, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
					}
					else
					{
						resource.m_image = VulkanApi::CreateImage(device, Extent, 1u, VK_IMAGE_TYPE_2D, Format,
							resource.m_tiling, ImageUsage, VK_SHARING_MODE_EXCLUSIVE, VK_SAMPLE_COUNT_1_BIT,
							VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
						native->ImageMemoryBarrier(resource.m_image, Format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
						VkClearColorValue color{};
						for (size_t channel = 0u; channel < 4u; ++channel) color.float32[channel] = resource.m_expected[channel] / 255.0f;
						const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
						// Transfer-only images need no image view. Retain the image for the recorded clear.
						vkCmdClearColorImage(*native, *resource.m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1u, &range);
						native->AddDependency(resource.m_image);
						native->ImageMemoryBarrier(resource.m_image, Format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
					}
				}
				if (resource.m_isBuffer)
				{
					memories[i] = *resource.m_buffer->GetMemoryDevice();
					requirements[i] = resource.m_buffer->GetMemoryRequirements();
					bufferOffsets[i] = (*resource.m_buffer->GetMemoryPtr()).m_offset;
					if (bufferOffsets[i] % requirements[i].alignment != 0u)
						allocationFailure = std::format("{} offset does not satisfy its buffer alignment", resource.m_name);
				}
				else
				{
					memories[i] = *resource.m_image->GetMemoryDevice();
					requirements[i] = resource.m_image->GetMemoryRequirements();
				}
				evidence += std::format("round {} {}: memory={:#x}, bits={:#x}, size={}, alignment={}", round,
					resource.m_name, uint64_t(memories[i]), requirements[i].memoryTypeBits, requirements[i].size, requirements[i].alignment);
				evidence += resource.m_isBuffer ? std::format(", offset={}; ", bufferOffsets[i]) : ", offset=not exposed; ";
				readbacks[i] = driver->CreateBuffer(ByteCount, EBufferUsageBit::BufferTransferDst_Bit, hostMemory);
			}

			for (size_t i = 0u; i < resources.size(); ++i)
			{
				if (memories[i] == VK_NULL_HANDLE) continue;
				for (size_t j = i + 1u; j < resources.size(); ++j)
				{
					if (memories[i] != memories[j]) continue;
					if (resources[i].m_tiling != resources[j].m_tiling && policyFailure.empty())
					{
						policyFailure = std::format("pool separation policy failed: {} and {} share VkDeviceMemory; bufferImageGranularity={}. "
							"Image offsets are not exposed; shared backing alone is not proof of a Vulkan granularity violation",
							resources[i].m_name, resources[j].m_name, properties.limits.bufferImageGranularity);
					}
					if (resources[i].m_isBuffer && resources[j].m_isBuffer &&
						bufferOffsets[i] < bufferOffsets[j] + requirements[j].size &&
						bufferOffsets[j] < bufferOffsets[i] + requirements[i].size)
						allocationFailure = "simultaneously live buffer allocations overlap";
				}
			}
			native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			for (size_t i = 0u; i < resources.size(); ++i)
			{
				if (!readbacks[i]) continue;
				auto& resource = resources[i];
				if (resource.m_isBuffer)
					native->CopyBuffer(resource.m_buffer->GetBufferMemoryPtr(), *readbacks[i]->m_vulkan.m_buffer, ByteCount);
				else
					native->CopyImageToBuffer(*readbacks[i]->m_vulkan.m_buffer, resource.m_image, Extent.width, Extent.height, Extent.depth);
			}
			native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
			commands->EndCommandList(cmd);
			auto fence = RHIFencePtr::Make();
			if (!driver->SubmitCommandList(cmd, fence)) return "device memory pool readback submission failed";
			fence->Wait(5000000000ull);
			if (!fence->IsFinished()) return "device memory pool readback fence exceeded five seconds";
			// Drop command/staging dependencies before releasing the selected resources below.
			native->Reset();
			fence->ClearDependencies();
			if (!allocationFailure.empty()) return allocationFailure;
			for (size_t i = 0u; i < resources.size(); ++i)
			{
				if (!readbacks[i]) continue;
				const auto* actual = static_cast<const uint8_t*>(readbacks[i]->GetPointer());
				for (size_t byte = 0u; byte < ByteCount; ++byte)
				{
					if (actual[byte] != resources[i].m_expected[byte])
						return std::format("round {} {} byte {}: expected {}, got {}", round, resources[i].m_name,
							byte, resources[i].m_expected[byte], actual[byte]);
				}
				++checkedReadbacks;
			}
			if (round < 2u)
			{
				resources[3].m_buffer.Clear();
				resources[round == 0u ? 1u : 4u].m_image.Clear();
				resources[round == 0u ? 2u : 5u].m_image.Clear();
			}
		}
		evidence += std::format("{} complete {}-byte readbacks passed across 3 rounds and 2 release/recreate cycles; "
			"no exact offset reuse assumed; ", checkedReadbacks, ByteCount);
		retainedBuffer = resources[0].m_buffer;
		return policyFailure;
	}
}

void DeviceMemoryPoolTestComponent::Tick(float)
{
	if (IsFinished()) return;
	if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 60000)
	{
		MarkFailed("device memory pool validation did not finish within 60 seconds");
		return;
	}
	if (!m_linearBuffer)
	{
		if (!m_validation)
		{
			m_validation = Tasks::CreateTaskWithResult<ValidationResult>("Device memory pool GPU validation", []()
			{
				ValidationResult result;
				result.m_error = ValidateMemoryPools(result.m_evidence, result.m_linearBuffer);
				if (result.m_error.empty()) result.m_error = ValidatePoolSelection(result.m_linearBuffer, result.m_evidence);
				if (result.m_error.empty()) result.m_error = ValidateMappedMemoryCopy(result.m_evidence);
				return result;
			}, EThreadType::RHI);
			m_validation->Run();
			return;
		}
		if (!m_validation->IsFinished()) return;
		const auto& result = m_validation->GetResult();
		AddJournalEvent("DeviceMemoryPoolEvidence", result.m_evidence);
		if (!result.m_error.empty()) { MarkFailed(result.m_error); return; }
		m_linearBuffer = result.m_linearBuffer;
		m_validation.Clear();
	}
	if (m_depthSnapshot)
	{
		if (!m_depthSnapshot->IsFinished()) return;
		auto snapshot = m_depthSnapshot->GetResult();
		m_depthSnapshot.Clear();
		if (!snapshot.m_error.empty())
		{
			AddJournalEvent("DeviceMemoryPoolDepth", snapshot.m_evidence);
			MarkFailed(snapshot.m_error);
			return;
		}
		const bool changed = snapshot.m_image != m_previousDepth &&
			(snapshot.m_extent.width != m_previousExtent.width || snapshot.m_extent.height != m_previousExtent.height);
		if (!m_previousDepth || changed)
		{
			AddJournalEvent("DeviceMemoryPoolDepth", std::format("resize {}: {}", m_resizeCount, snapshot.m_evidence));
			m_previousDepth = snapshot.m_image;
			m_previousExtent = snapshot.m_extent;
			if (m_resizeCount == 3u)
			{
				m_previousDepth.Clear();
				m_linearBuffer.Clear();
				AddJournalEvent("DeviceMemoryPoolResizeEvidence",
					"initial depth and 3 changed native depth images/extents stayed separate from linear buffer memory; "
					"older depth snapshots released between requests; normal world exit/shutdown follows");
				MarkPassed();
				return;
			}
			// Tick runs on Main. The normal device recovery owns recreation on Render.
			auto* window = App::GetMainWindowPlatform();
			if (!window) { MarkFailed("resize validation has no application window"); return; }
			const bool useLargerSize = window->GetWidth() == 800 && window->GetHeight() == 600;
			window->ChangeWindowSize(useLargerSize ? 960 : 800, useLargerSize ? 720 : 600, false);
			++m_resizeCount;
		}
	}
	m_depthSnapshot = Tasks::CreateTaskWithResult<DepthSnapshot>("Device memory pool depth snapshot", [linearBuffer = m_linearBuffer]() mutable
	{
		DepthSnapshot snapshot;
		auto target = Renderer::GetDriver()->GetDepthBuffer();
		if (!target || !target->m_vulkan.m_image)
		{
			snapshot.m_error = "resize validation has no native depth image";
			return snapshot;
		}
		snapshot.m_image = target->m_vulkan.m_image;
		snapshot.m_extent = { snapshot.m_image->m_extent.width, snapshot.m_image->m_extent.height };
		const auto requirements = snapshot.m_image->GetMemoryRequirements();
		const VkDeviceMemory memory = *snapshot.m_image->GetMemoryDevice();
		const VkDeviceMemory linearMemory = *linearBuffer->GetMemoryDevice();
		snapshot.m_evidence = std::format("native image={:#x}, pixels={}x{}, memory={:#x}, linear memory={:#x}, bits={:#x}, size={}, alignment={}",
			uint64_t(VkImage(*snapshot.m_image)), snapshot.m_extent.width, snapshot.m_extent.height,
			uint64_t(memory), uint64_t(linearMemory), requirements.memoryTypeBits, requirements.size, requirements.alignment);
		if (memory == linearMemory) snapshot.m_error = "resized depth and linear buffer share device memory: pool separation policy failed";
		return snapshot;
	}, EThreadType::Render);
	m_depthSnapshot->Run();
}
