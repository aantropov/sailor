#include "Components/Tests/DescriptorPreparationTestComponent.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "GraphicsDriver/Vulkan/VulkanDescriptors.h"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "GraphicsDriver/Vulkan/VulkanPipeline.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Renderer.h"
#include <array>
#include <cstring>
#include <format>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	using Values = std::array<uint32_t, 4>;
	constexpr std::array<Values, 3> InputValues{
		Values{ 17u, 31u, 0x12345678u, 0xfedcba98u },
		Values{ 101u, 307u, 0xc001d00du, 0x5a6b7c8du },
		Values{ 53u, 79u, 0x87654321u, 0xabcddcbau }
	};

	std::string ValidateDescriptors(ShaderSetPtr shader, bool& outVariableDescriptorsTested)
	{
		auto& driver = Renderer::GetDriver();
		auto* nativeDriver = driver.DynamicCast<VulkanGraphicsDriver>();
		auto pipeline = nativeDriver->GetOrAddComputePipeline(shader->GetComputeShaderRHI());
		if (!pipeline || !pipeline->IsCompiled() || pipeline->m_layout->m_descriptionSetLayouts.Num() != 2u)
			return "descriptor validation compute pipeline did not expose two sets";
		auto inputLayout = pipeline->m_layout->m_descriptionSetLayouts[0];
		const auto& bindings = inputLayout->m_descriptorSetLayoutBindings;
		if (bindings.Num() != 1u || bindings[0].binding != 0u || bindings[0].descriptorCount != 2u ||
			bindings[0].descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			return "descriptor validation shader did not expose the two-element storage buffer array";

		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto pool = device->GetCurrentThreadContext().m_descriptorPool;
		const EMemoryPropertyFlags hostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
		std::array<RHIBufferPtr, 3> buffers;
		for (uint32_t i = 0u; i < buffers.size(); ++i)
		{
			buffers[i] = driver->CreateBuffer(sizeof(Values),
				EBufferUsageBit::StorageBuffer_Bit | EBufferUsageBit::UniformBuffer_Bit, hostMemory);
			std::memcpy(buffers[i]->GetPointer(), InputValues[i].data(), sizeof(Values));
		}
		const auto descriptor = [&](uint32_t buffer, uint32_t element, uint32_t binding = 0u,
			EShaderBindingType type = EShaderBindingType::StorageBuffer) -> VulkanDescriptorPtr
		{
			return VulkanDescriptorBufferPtr::Make(binding, element,
				buffers[buffer]->m_vulkan.m_buffer.m_ptr.m_buffer, 0u, sizeof(Values), type);
		};
		const auto makeSet = [&](TVector<VulkanDescriptorPtr> writes)
		{
			return VulkanDescriptorSetPtr::Make(device, pool, inputLayout, std::move(writes));
		};
		auto valueA = descriptor(0u, 0u);
		auto neighbor = descriptor(1u, 1u);
		auto valueC = descriptor(2u, 0u);
		auto oldSet = makeSet({ valueA, neighbor });
		if (!oldSet->TryCompile()) return "valid original descriptor set could not be compiled";
		const VkDescriptorSet oldHandle = *oldSet;

		auto uncompiled = VulkanBufferPtr::Make(device, sizeof(Values),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE);
		VulkanDescriptorPtr missingHandle = VulkanDescriptorBufferPtr::Make(0u, 1u, uncompiled,
			0u, sizeof(Values), EShaderBindingType::StorageBuffer);
		auto candidate = makeSet({ valueA, missingHandle });
		// Old runtimes skip this write and publish a partial set. Stop before testing unsafe appends there.
		if (candidate->TryCompile() || candidate->IsCompiled())
			return "mixed valid/null-native descriptor writes published a partial set instead of rejecting preparation";
		candidate->m_descriptors[0] = valueC;
		candidate->m_descriptors[1] = neighbor;
		if (!candidate->TryCompile() || !candidate->IsCompiled() || static_cast<VkDescriptorSet>(*candidate) == oldHandle)
			return "correcting a rejected descriptor candidate did not produce a distinct usable set";

		auto empty = makeSet({});
		if (!empty->TryCompile() || !empty->IsCompiled() || !empty->m_descriptors.IsEmpty())
			return "an empty supplied descriptor list was rejected";
		auto sparse = makeSet({ valueA });
		if (!sparse->TryCompile() || !sparse->IsCompiled()) return "an omitted unused array slot was rejected";
		const VkDescriptorSet sparseHandle = *sparse;
		struct InvalidWrite
		{
			const char* m_name;
			VulkanDescriptorPtr m_descriptor;
		};
		const std::array<InvalidWrite, 6> invalidWrites{
			InvalidWrite{ "missing native handle", missingHandle },
			InvalidWrite{ "missing binding", descriptor(1u, 1u, 7u) },
			InvalidWrite{ "wrong type", descriptor(1u, 1u, 0u, EShaderBindingType::UniformBuffer) },
			InvalidWrite{ "array end", descriptor(1u, 2u) },
			InvalidWrite{ "overflowing array end", descriptor(1u, UINT32_MAX) },
			InvalidWrite{ "empty write", VulkanDescriptorPtr::Make(0u, 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) }
		};
		for (const auto& invalid : invalidWrites)
		{
			auto rejected = makeSet({ valueA, invalid.m_descriptor });
			if (rejected->TryCompile() || rejected->IsCompiled())
				return std::format("{} was accepted while compiling a descriptor candidate", invalid.m_name);
			if (sparse->UpdateDescriptor(invalid.m_descriptor) || static_cast<VkDescriptorSet>(*sparse) != sparseHandle ||
				sparse->m_descriptors.Num() != 1u || sparse->m_descriptors[0] != valueA)
				return std::format("{} changed the published set during append", invalid.m_name);
		}
		// No shader uses the empty or sparse sets until both dynamically used elements are populated.
		if (!sparse->UpdateDescriptor(neighbor) || sparse->m_descriptors.Num() != 2u ||
			static_cast<VkDescriptorSet>(*sparse) != sparseHandle)
			return "appending a valid descriptor after rejection did not preserve the set";
		if (oldSet->UpdateDescriptor(valueC) || static_cast<VkDescriptorSet>(*oldSet) != oldHandle ||
			oldSet->m_descriptors.Num() != 2u || oldSet->m_descriptors[0] != valueA || oldSet->m_descriptors[1] != neighbor)
			return "replacing an occupied slot changed the retained original set";

		auto image = VulkanImagePtr::Make(device);
		image->m_format = VK_FORMAT_R8G8B8A8_UNORM;
		image->m_extent = { 1u, 1u, 1u };
		image->m_mipLevels = image->m_arrayLayers = 1u;
		image->m_usage = VK_IMAGE_USAGE_STORAGE_BIT;
		auto imageView = VulkanImageViewPtr::Make(device, image);
		VulkanDescriptorPtr missingImage = VulkanDescriptorStorageImagePtr::Make(0u, 0u, imageView);
		auto imageLayout = VulkanDescriptorSetLayoutPtr::Make(device, TVector<VkDescriptorSetLayoutBinding>{
			{ 0u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } });
		auto imageCandidate = VulkanDescriptorSetPtr::Make(device, pool, imageLayout, TVector<VulkanDescriptorPtr>{ missingImage });
		if (imageCandidate->TryCompile() || imageCandidate->IsCompiled())
			return "a null native image view was accepted during descriptor preparation";
		auto emptyImageSet = VulkanDescriptorSetPtr::Make(device, pool, imageLayout, TVector<VulkanDescriptorPtr>{});
		if (!emptyImageSet->TryCompile()) return "empty storage-image set could not be compiled";
		const VkDescriptorSet imageHandle = *emptyImageSet;
		if (emptyImageSet->UpdateDescriptor(missingImage) || !emptyImageSet->m_descriptors.IsEmpty() ||
			static_cast<VkDescriptorSet>(*emptyImageSet) != imageHandle)
			return "a null native image view changed the published set during append";

		VkPhysicalDeviceVulkan12Features core12{};
		core12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		VkPhysicalDeviceFeatures2 features{};
		features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features.pNext = &core12;
		vkGetPhysicalDeviceFeatures2(device->GetPhysicalDevice(), &features);
		// The device enables these supported features; variable layouts use all three flags.
		if (core12.descriptorBindingVariableDescriptorCount && core12.descriptorBindingPartiallyBound &&
			core12.descriptorBindingUpdateUnusedWhilePending)
		{
			auto variableLayout = VulkanDescriptorSetLayoutPtr::Make(device, TVector<VkDescriptorSetLayoutBinding>{
				{ 0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } }, 0);
			auto variable = VulkanDescriptorSetPtr::Make(device, pool, variableLayout,
				TVector<VulkanDescriptorPtr>{ valueA, descriptor(1u, 2u) }, 2u);
			if (variable->TryCompile() || variable->IsCompiled())
				return "variable descriptor preparation used layout maximum instead of allocated capacity";
			variable->m_descriptors.Resize(1u);
			if (!variable->TryCompile() || !variable->IsCompiled() || variable->GetVariableDescriptorCount() != 2u)
				return "variable descriptor retry with an unused allocated slot failed";
			const VkDescriptorSet variableHandle = *variable;
			for (uint32_t element : { 2u, 3u })
			{
				auto outsideAllocation = descriptor(1u, element);
				auto rejected = VulkanDescriptorSetPtr::Make(device, pool, variableLayout,
					TVector<VulkanDescriptorPtr>{ valueA, outsideAllocation }, 2u);
				if (rejected->TryCompile() || rejected->IsCompiled() || variable->UpdateDescriptor(outsideAllocation) ||
					static_cast<VkDescriptorSet>(*variable) != variableHandle || variable->m_descriptors.Num() != 1u ||
					variable->m_descriptors[0] != valueA)
					return std::format("variable descriptor element {} exceeded allocated capacity 2", element);
			}
			if (!variable->UpdateDescriptor(neighbor) || variable->m_descriptors.Num() != 2u ||
				static_cast<VkDescriptorSet>(*variable) != variableHandle)
				return "valid append into the variable descriptor allocation failed";
			outVariableDescriptorsTested = true;
		}

		constexpr size_t OutputSize = 2u * sizeof(Values);
		std::array<RHIBufferPtr, 4> outputs;
		std::array<RHIBufferPtr, 4> readbacks;
		std::array<VulkanDescriptorSetPtr, 4> outputSets;
		for (uint32_t i = 0u; i < outputs.size(); ++i)
		{
			outputs[i] = driver->CreateBuffer(OutputSize,
				EBufferUsageBit::StorageBuffer_Bit | EBufferUsageBit::BufferTransferSrc_Bit, EMemoryPropertyBit::DeviceLocal);
			readbacks[i] = driver->CreateBuffer(OutputSize, EBufferUsageBit::BufferTransferDst_Bit, hostMemory);
			std::memset(readbacks[i]->GetPointer(), 0xa7, OutputSize);
			VulkanDescriptorPtr output = VulkanDescriptorBufferPtr::Make(0u, 0u,
				outputs[i]->m_vulkan.m_buffer.m_ptr.m_buffer, 0u, OutputSize, EShaderBindingType::StorageBuffer);
			outputSets[i] = VulkanDescriptorSetPtr::Make(device, pool, pipeline->m_layout->m_descriptionSetLayouts[1],
				TVector<VulkanDescriptorPtr>{ output });
			if (!outputSets[i]->TryCompile()) return "descriptor readback output set could not be compiled";
		}
		auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		auto native = cmd->m_vulkan.m_commandBuffer;
		native->BeginCommandList(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
		native->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
		native->BindPipeline(pipeline);
		native->AddDependency(shader->GetComputeShaderRHI());
		for (uint32_t i = 0u; i < outputs.size(); ++i)
		{
			auto inputSet = i == 3u ? sparse : (i == 1u ? candidate : oldSet);
			native->BindDescriptorSet(pipeline->m_layout, { inputSet, outputSets[i] },
				VK_PIPELINE_BIND_POINT_COMPUTE);
			native->Dispatch(1u, 1u, 1u);
			native->MemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			native->CopyBuffer(*outputs[i]->m_vulkan.m_buffer, *readbacks[i]->m_vulkan.m_buffer, OutputSize);
		}
		native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
		native->EndCommandList();
		// Recorded dependencies, not these caller handles, retain the sets through submission.
		oldSet.Clear();
		candidate.Clear();
		sparse.Clear();
		for (auto& output : outputSets) output.Clear();
		for (auto& output : outputs) output.Clear();
		auto fence = RHIFencePtr::Make();
		if (!driver->SubmitCommandList(cmd, fence)) return "descriptor validation submission failed";
		fence->Wait(5000000000ull);
		if (!fence->IsFinished()) return "descriptor validation fence exceeded five seconds";
		native->Reset();
		fence->ClearDependencies();
		for (uint32_t scenario = 0u; scenario < readbacks.size(); ++scenario)
		{
			const auto* actual = static_cast<const uint32_t*>(readbacks[scenario]->GetPointer());
			for (uint32_t word = 0u; word < 8u; ++word)
			{
				const uint32_t input = word < 4u ? (scenario == 1u ? 2u : 0u) : 1u;
				const uint32_t expected = InputValues[input][word % 4u];
				if (actual[word] != expected)
					return std::format("descriptor scenario {} word {}: expected {}, got {}", scenario, word, expected, actual[word]);
			}
		}
		return {};
	}
}

void DescriptorPreparationTestComponent::Tick(float)
{
	if (IsFinished()) return;
	if (m_validation)
	{
		if (!m_validation->IsFinished()) return;
		const auto& result = m_validation->GetResult();
		if (!result.m_error.empty()) { MarkFailed(result.m_error); return; }
		AddJournalEvent("DescriptorPreparationEvidence",
			"All-or-none native preparation, buffer/image handle rejection, same-candidate retry, empty/sparse sets and 4 x 8-word GPU readbacks (A/C/A and appended set, each with neighbor) passed");
		AddJournalEvent("DescriptorVariableCapacity", result.m_bVariableDescriptorsTested ?
			"Layout capacity 4, allocation capacity 2: rejected elements 2/3, sparse retry and valid append passed" :
			"Skipped: device lacks variable-count, partially-bound or update-unused descriptor support");
		MarkPassed();
		return;
	}
	if (!m_shader)
	{
		auto* registry = App::GetSubmodule<AssetRegistry>();
		if (auto info = registry->GetAssetInfoPtr("Tests/Shaders/DescriptorPreparation.shader"))
			App::GetSubmodule<ShaderCompiler>()->LoadShader(info->GetFileId(), m_shader);
	}
	if (m_shader && m_shader->IsReady())
	{
		m_validation = Tasks::CreateTaskWithResult<ValidationResult>("Native descriptor preparation validation",
			[shader = m_shader]()
			{
				ValidationResult result;
				result.m_error = ValidateDescriptors(shader, result.m_bVariableDescriptorsTested);
				return result;
			}, EThreadType::RHI);
		m_validation->Run();
	}
	else if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 30000)
	{
		MarkFailed("descriptor validation shader did not become ready within 30 seconds");
	}
}
