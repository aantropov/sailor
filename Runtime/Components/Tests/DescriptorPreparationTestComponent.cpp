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
#include "RHI/Material.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include <algorithm>
#include <array>
#include <chrono>
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

	std::string MeasureBindingUpdates(std::string& timings)
	{
		auto& driver = Renderer::GetDriver();
		auto relocated = driver->CreateShaderBindings();
		ShaderLayoutBinding reflected;
		reflected.m_name = "reflected";
		reflected.m_binding = 1u;
		reflected.m_type = EShaderBindingType::CombinedImageSampler;
		relocated->SetLayoutShaderBindings({ reflected });
		auto texture = driver->GetDefaultTexture();
		auto originalSampler = driver->AddSamplerToShaderBindings(relocated, "sampler", texture, 0u);
		if (!originalSampler || driver->AddSamplerToShaderBindings(relocated, "sampler", texture, 1u) != originalSampler ||
			relocated->GetLayoutBindings().Num() != 1u || relocated->GetLayoutBindings()[0].m_binding != 1u ||
			relocated->GetLayoutBindings()[0].m_name != "sampler")
			return "moving a sampler into a reflected slot left duplicate layout entries";

		auto bindings = driver->CreateShaderBindings();
		for (uint32_t slot = 0; slot < 24; ++slot)
			if (!driver->AddSamplerToShaderBindings(bindings, std::format("slot{}", slot), texture, slot))
				return "binding update benchmark could not prepare its 24-slot set";
		const auto original = bindings->GetOrAddShaderBinding("slot7");
		const auto source = bindings->GetOrAddShaderBinding("slot8");
		std::array<double, 2> medians{};
		for (uint32_t kind = 0; kind < medians.size(); ++kind)
		{
			std::array<double, 17> samples{};
			const auto revision = bindings->GetDescriptorRevision();
			for (auto& sample : samples)
			{
				const auto start = std::chrono::steady_clock::now();
				for (uint32_t i = 0; i < 256; ++i)
				{
					const auto updated = kind == 0 ?
						driver->AddSamplerToShaderBindings(bindings, "slot7", texture, 7) :
						driver->AddShaderBinding(bindings, source, "slot7", 7);
					if (updated != original) return "repeated update replaced the published binding identity";
				}
				sample = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 256;
			}
			if (bindings->GetDescriptorRevision() != revision + samples.size() * 256 ||
				bindings->GetLayoutBindings().Num() != 24 || bindings->GetShaderBindings().Num() != 24 ||
				!bindings->m_vulkan.m_descriptorSet || !bindings->m_vulkan.m_descriptorSet->IsCompiled())
				return "repeated updates did not preserve one complete descriptor generation per call";
			std::sort(samples.begin() + 1, samples.end());
			medians[kind] = (samples[8] + samples[9]) / 2;
		}
		timings = std::format("24 slots, 16 x 256 warmed updates: AddSampler median {:.3f} us/call; AddShaderBinding median {:.3f} us/call. CPU-only descriptor preparation, not frame time.",
			medians[0], medians[1]);
		return {};
	}

	std::string ValidateLayoutOrder()
	{
		const VkDescriptorSetLayoutBinding storage{ 0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		const VkDescriptorSetLayoutBinding uniform{ 1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
		auto forward = VulkanDescriptorSetLayoutPtr::Make(VulkanDevicePtr{}, TVector<VkDescriptorSetLayoutBinding>{ storage, uniform });
		auto reverse = VulkanDescriptorSetLayoutPtr::Make(VulkanDevicePtr{}, TVector<VkDescriptorSetLayoutBinding>{ uniform, storage });
		if (!(*forward == *reverse) || forward->GetHash() != reverse->GetHash())
			return "equivalent native layouts with opposite binding order were rejected before submission";
		return {};
	}

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
				buffers[buffer]->m_vulkan.m_buffer->Get().m_ptr.m_buffer, 0u, sizeof(Values), type);
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

			auto mixedLayout = VulkanDescriptorSetLayoutPtr::Make(device, TVector<VkDescriptorSetLayoutBinding>{
				{ 7u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
				{ 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } }, 7);
			auto mixed = VulkanDescriptorSetPtr::Make(device, pool, mixedLayout, TVector<VulkanDescriptorPtr>{
				descriptor(0u, 0u, 7u), descriptor(1u, 0u, 0u, EShaderBindingType::UniformBuffer) }, 2u);
			if (!mixed->TryCompile() || !mixed->IsCompiled() || mixed->GetVariableDescriptorCount() != 2u ||
				mixedLayout->GetVariableDescriptorBinding() != 7 || mixedLayout->m_descriptorSetLayoutBindings[1].binding != 7u)
				return "reversed mixed layout lost its numeric variable binding or native allocation capacity";
			const VkDescriptorSet mixedHandle = *mixed;
			if (!mixed->UpdateDescriptor(descriptor(2u, 1u, 7u)) || mixed->m_descriptors.Num() != 3u ||
				mixed->UpdateDescriptor(descriptor(2u, 2u, 7u)) || mixed->m_descriptors.Num() != 3u ||
				static_cast<VkDescriptorSet>(*mixed) != mixedHandle)
				return "mixed fixed/variable layout did not preserve allocated append bounds";
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
				outputs[i]->m_vulkan.m_buffer->Get().m_ptr.m_buffer, 0u, OutputSize, EShaderBindingType::StorageBuffer);
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
			native->CopyBuffer(*outputs[i]->m_vulkan.m_buffer->Get(), *readbacks[i]->m_vulkan.m_buffer->Get(), OutputSize);
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

	struct PublishedState
	{
		struct Binding
		{
			std::string m_name;
			RHIShaderBindingPtr m_binding;
			ShaderLayoutBinding m_layout;
			TVector<RHITexturePtr> m_textures;
			decltype(RHIShaderBinding::m_vulkan) m_native;
			TMemoryPtr<VulkanBufferMemoryPtr> m_range;
			size_t m_hash = 0u;
		};
		RHIShaderBindingSetPtr m_set;
		TVector<Binding> m_bindings;
		TVector<ShaderLayoutBinding> m_layouts;
		VulkanDescriptorSetPtr m_native;
		uint64_t m_revision;
		uint32_t m_capacity;
		size_t m_hash;
		bool m_needsStorage;

		explicit PublishedState(RHIShaderBindingSetPtr set) : m_set(set), m_layouts(set->GetLayoutBindings()),
			m_native(set->m_vulkan.m_descriptorSet), m_revision(set->GetDescriptorRevision()),
			m_capacity(set->GetVariableDescriptorCount()), m_hash(set->GetCompatibilityHashCode()),
			m_needsStorage(set->NeedsStorageBuffer())
		{
			for (const auto& entry : set->GetShaderBindings())
			{
				auto binding = entry.m_second;
				Binding state{ entry.m_first, binding, binding->GetLayout(), binding->GetTextureBindings(), binding->m_vulkan };
				if (state.m_native.m_valueBinding) state.m_range = state.m_native.m_valueBinding->Get();
				state.m_hash = binding->GetCompatibilityHash();
				m_bindings.Emplace(std::move(state));
			}
		}

		bool Unchanged() const
		{
			if (m_set->GetShaderBindings().Num() != m_bindings.Num() || m_set->GetLayoutBindings() != m_layouts ||
				m_set->m_vulkan.m_descriptorSet != m_native || m_set->GetDescriptorRevision() != m_revision ||
				m_set->GetVariableDescriptorCount() != m_capacity || m_set->GetCompatibilityHashCode() != m_hash ||
				m_set->NeedsStorageBuffer() != m_needsStorage) return false;
			for (const auto& state : m_bindings)
			{
				RHIShaderBindingPtr binding;
				if (!m_set->GetShaderBindings().TryGet(state.m_name, binding) || binding != state.m_binding) return false;
				const auto& native = binding->m_vulkan;
				const auto& layout = native.m_descriptorSetLayout;
				const auto& oldLayout = state.m_native.m_descriptorSetLayout;
				if (binding->GetLayout() != state.m_layout || binding->GetLayout().m_textureType != state.m_layout.m_textureType ||
					binding->GetTextureBindings() != state.m_textures || binding->GetCompatibilityHash() != state.m_hash ||
					native.m_valueBinding != state.m_native.m_valueBinding || native.m_storageInstanceIndex != state.m_native.m_storageInstanceIndex ||
					native.m_bBindSsboWithOffset != state.m_native.m_bBindSsboWithOffset ||
					layout.binding != oldLayout.binding || layout.descriptorType != oldLayout.descriptorType ||
					layout.descriptorCount != oldLayout.descriptorCount || layout.stageFlags != oldLayout.stageFlags ||
					layout.pImmutableSamplers != oldLayout.pImmutableSamplers) return false;
				if (native.m_valueBinding && native.m_valueBinding->Get() != state.m_range) return false;
			}
			return true;
		}
	};

	const EMemoryPropertyFlags PublicationHostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
	struct PublicationReadback
	{
		RHIBufferPtr m_buffer;
		TVector<uint32_t> m_expected;
		std::string m_name;
	};

	TVector<uint32_t> Expected(std::initializer_list<Values> values)
	{
		TVector<uint32_t> result;
		for (const auto& value : values) result.AddRange(value.data(), value.size());
		return result;
	}

	RHIBufferPtr CreatePublicationBuffer(const Values& values, EBufferUsageFlags usage = EBufferUsageBit::StorageBuffer_Bit)
	{
		auto result = Renderer::GetDriver()->CreateBuffer(sizeof(values), usage | EBufferUsageBit::BufferTransferSrc_Bit,
			PublicationHostMemory);
		std::memcpy(result->GetPointer(), values.data(), sizeof(values));
		return result;
	}

	RHITexturePtr CreatePublicationTexture(const Values& values)
	{
		std::array<uint8_t, sizeof(Values)> pixels;
		for (uint32_t i = 0u; i < values.size(); ++i)
			for (uint32_t channel = 0u; channel < 4u; ++channel) pixels[i * 4u + channel] = uint8_t(values[i] >> (channel * 8u));
		return Renderer::GetDriver()->CreateTexture(pixels.data(), pixels.size(), glm::ivec3(4, 1, 1), 1u,
			ETextureType::Texture2D, ETextureFormat::R8G8B8A8_UNORM, ETextureFiltration::Nearest, ETextureClamping::Clamp,
			ETextureUsageBit::Sampled_Bit | ETextureUsageBit::Storage_Bit | ETextureUsageBit::TextureTransferDst_Bit);
	}

	std::string RecordPublication(RHICommandListPtr cmd, ShaderSetPtr shader, RHIShaderBindingSetPtr inputs,
		TVector<uint32_t> expected, const char* name, TVector<PublicationReadback>& readbacks, bool requireProjection = false)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const size_t size = expected.Num() * sizeof(uint32_t);
		auto output = driver->CreateBuffer(size, EBufferUsageBit::StorageBuffer_Bit | EBufferUsageBit::BufferTransferSrc_Bit,
			EMemoryPropertyBit::DeviceLocal);
		auto outputBindings = driver->CreateShaderBindings();
		if (!driver->AddBufferToShaderBindings(outputBindings, output, "outputValue", 0u)) return "readback output binding failed";
		if (requireProjection)
		{
			auto* nativeDriver = driver.DynamicCast<VulkanGraphicsDriver>();
			TVector<uint32_t> capacities{ inputs->GetVariableDescriptorCount() };
			auto pipeline = nativeDriver->GetOrAddComputePipeline(shader->GetComputeShaderRHI(), &capacities);
			if (!pipeline || !pipeline->IsCompiled() || pipeline->m_layout->m_descriptionSetLayouts.Num() != 2u)
				return "sparse compute pipeline did not expose two sets";
			auto inputLayout = pipeline->m_layout->m_descriptionSetLayouts[0];
			if (!inputLayout->HasVariableDescriptorBinding() || inputLayout->GetVariableDescriptorBinding() != 1 ||
				inputLayout->m_descriptorSetLayoutBindings.Num() != 1u ||
				inputLayout->m_descriptorSetLayoutBindings[0].binding != 1u ||
				inputLayout->m_descriptorSetLayoutBindings[0].descriptorCount < 4u)
				return "sparse compute pipeline did not expose variable binding 1 with capacity for slot 3";
			if (nativeDriver->IsCompatible(pipeline->m_layout, { inputs, outputBindings })[0])
				return "sparse fixture did not require a compatible projection";
			const PublishedState before(inputs);
			auto projected = nativeDriver->GetCompatibleDescriptorSets(pipeline->m_layout, { inputs, outputBindings });
			if (projected.Num() != 2u || !projected[0] || !projected[0]->IsCompiled() ||
				projected[0]->GetVariableDescriptorCount() < 4u || projected[0] == inputs->m_vulkan.m_descriptorSet ||
				!before.Unchanged()) return "compatible sparse projection was not prepared independently";
		}
		// Managed ranges also remain owned if the bounded fence wait times out.
		cmd->m_vulkan.m_commandBuffer->AddDependency(inputs);
		commands->Dispatch(cmd, shader->GetComputeShaderRHI(), 1u, 1u, 1u, { inputs, outputBindings });
		PublicationReadback readback{ driver->CreateBuffer(size, EBufferUsageBit::BufferTransferDst_Bit, PublicationHostMemory),
			std::move(expected), name };
		std::memset(readback.m_buffer->GetPointer(), 0xa7, size);
		auto native = cmd->m_vulkan.m_commandBuffer;
		native->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		native->MemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		native->CopyBuffer(*output->m_vulkan.m_buffer->Get(), *readback.m_buffer->m_vulkan.m_buffer->Get(), size);
		readbacks.Emplace(std::move(readback));
		return {};
	}

	std::string FinishPublication(RHICommandListPtr cmd, TVector<PublicationReadback>& readbacks)
	{
		auto native = cmd->m_vulkan.m_commandBuffer;
		native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
		Renderer::GetDriverCommands()->EndCommandList(cmd);
		auto fence = RHIFencePtr::Make();
		if (!Renderer::GetDriver()->SubmitCommandList(cmd, fence)) return "binding publication submission failed";
		fence->Wait(5000000000ull);
		if (!fence->IsFinished()) return "binding publication fence exceeded five seconds";
		native->Reset();
		fence->ClearDependencies();
		for (auto& readback : readbacks)
		{
			const auto* actual = static_cast<const uint32_t*>(readback.m_buffer->GetPointer());
			for (size_t word = 0u; word < readback.m_expected.Num(); ++word)
				if (actual[word] != readback.m_expected[word])
					return std::format("{} word {}: expected {}, got {}", readback.m_name, word, readback.m_expected[word], actual[word]);
		}
		return {};
	}

	using ManagedBufferOwner = Memory::TManagedMemory<Memory::VulkanBufferMemoryPtr, VulkanBufferAllocator>;

	RHIShaderBindingPtr AddManagedSource(RHIShaderBindingSetPtr& bindings, const std::string& name, bool uniform)
	{
		auto& driver = Renderer::GetDriver();
		return uniform ? driver->AddBufferToShaderBindings(bindings, name, sizeof(Values), 1u, EShaderBindingType::UniformBuffer) :
			driver->AddSsboToShaderBindings(bindings, name, sizeof(Values), 1u, 1u, true);
	}

	enum class UploadRelease { Reset, Destroy, TwoCommands };

	std::string ValidateBareUploadOwners(bool uniform, UploadRelease release)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const std::string name = std::format("ManagedUploadGate_{}_{}", uniform ? "UBO" : "SSBO", static_cast<uint32_t>(release));
		auto inputs = driver->CreateShaderBindings();
		auto binding = AddManagedSource(inputs, name, uniform);
		if (!binding || !binding->m_vulkan.m_valueBinding) return name + ": managed A allocation failed";
		TWeakPtr<ManagedBufferOwner> weakA(binding->m_vulkan.m_valueBinding);
		const VkDescriptorSet originalNative = *inputs->m_vulkan.m_descriptorSet;
		const uint64_t revision = inputs->GetDescriptorRevision();
		std::array<RHICommandListPtr, 2> uploads;
		const size_t count = release == UploadRelease::TwoCommands ? 2u : 1u;
		for (size_t i = 0u; i < count; ++i)
		{
			uploads[i] = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(uploads[i], true);
			Values data = InputValues[0];
			commands->UpdateShaderBinding(uploads[i], binding, data.data(), sizeof(data));
			data.fill(0xdeadbeefu);
			commands->EndCommandList(uploads[i]);
		}
		const bool replaced = AddManagedSource(inputs, name, uniform) == binding &&
			inputs->GetDescriptorRevision() == revision + 1u &&
			static_cast<VkDescriptorSet>(*inputs->m_vulkan.m_descriptorSet) != originalNative;
		binding.Clear();
		inputs.Clear();
		// Old runtimes must reach only this out-of-line Reset, never inline native command fields or a submission.
		if (!replaced || !weakA.TryLock())
		{
			for (size_t i = 0u; i < count; ++i) uploads[i]->m_vulkan.m_commandBuffer->Reset();
			return name + (replaced ? ": bare upload lost A after same-binding replacement, before submission" :
				": B did not preserve binding identity and publish once");
		}
		if (release == UploadRelease::Destroy) uploads[0].Clear();
		else uploads[0]->m_vulkan.m_commandBuffer->Reset();
		if (count == 2u)
		{
			const bool retainedBySecond = static_cast<bool>(weakA.TryLock());
			uploads[1]->m_vulkan.m_commandBuffer->Reset();
			if (!retainedBySecond) return name + ": first reset released A while the second upload command remained";
		}
		if (weakA.TryLock()) return name + ": A remained owned after the last upload command was reset or destroyed";
		return {};
	}

	std::string ValidateBareUploadReadbacks(ShaderSetPtr shader, bool uniform)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto* nativeDriver = driver.DynamicCast<VulkanGraphicsDriver>();
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		const std::string name = uniform ? "ManagedUploadGpu_UBO" : "ManagedUploadGpu_SSBO";
		auto pipeline = nativeDriver->GetOrAddComputePipeline(shader->GetComputeShaderRHI());
		if (!pipeline || !pipeline->IsCompiled() || pipeline->m_layout->m_descriptionSetLayouts.Num() != 2u)
			return name + ": compute pipeline did not expose two sets";
		auto blocker = driver->CreateShaderBindings();
		if (!AddManagedSource(blocker, name, uniform)) return name + ": live allocation blocker failed";
		auto inputs = driver->CreateShaderBindings();
		auto binding = AddManagedSource(inputs, name, uniform);
		if (!binding || !binding->m_vulkan.m_valueBinding) return name + ": managed A allocation failed";
		const size_t alignment = uniform ? device->GetMinUboOffsetAlignment() : device->GetMinSsboOffsetAlignment();
		if (binding->GetBufferOffset() % alignment != 0u || (uniform && binding->GetBufferOffset() == 0u) ||
			(!uniform && !binding->m_vulkan.m_bBindSsboWithOffset))
			return name + ": managed source did not expose the required aligned offset";
		TWeakPtr<ManagedBufferOwner> weakA(binding->m_vulkan.m_valueBinding);
		const auto rangeA = *binding->m_vulkan.m_valueBinding->Get();
		const VkDescriptorSet originalNative = *inputs->m_vulkan.m_descriptorSet;
		const uint64_t revision = inputs->GetDescriptorRevision();
		auto neighbor = CreatePublicationBuffer(InputValues[1]);
		VulkanDescriptorPtr neighborDescriptor = VulkanDescriptorBufferPtr::Make(0u, 0u,
			neighbor->m_vulkan.m_buffer->Get().m_ptr.m_buffer, 0u, sizeof(Values), EShaderBindingType::StorageBuffer);
		const auto rawSet = [&](const Memory::VulkanBufferMemoryPtr& range)
		{
			// This overload owns only the backing buffer, not the managed reservation being tested.
			VulkanDescriptorPtr source = VulkanDescriptorBufferPtr::Make(1u, 0u, range.m_buffer, range.m_offset, range.m_size,
				uniform ? EShaderBindingType::UniformBuffer : EShaderBindingType::StorageBuffer);
			return VulkanDescriptorSetPtr::Make(device, device->GetCurrentThreadContext().m_descriptorPool,
				pipeline->m_layout->m_descriptionSetLayouts[0], TVector<VulkanDescriptorPtr>{ source, neighborDescriptor });
		};
		auto rawA = rawSet(rangeA);
		if (!rawA->TryCompile() || !VulkanApi::IsCompatible(pipeline->m_layout, rawA, 0u))
			return name + ": raw A descriptors did not match the shader";
		auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(cmd, true);
		auto native = cmd->m_vulkan.m_commandBuffer;
		native->BindPipeline(pipeline);
		native->AddDependency(shader->GetComputeShaderRHI());
		native->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		Values dataA = InputValues[0];
		dataA[1] ^= 0x13579bdfu;
		commands->UpdateShaderBinding(cmd, binding, dataA.data(), sizeof(dataA));
		dataA.fill(0xdeadbeefu);
		native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		uint32_t word = InputValues[0][1];
		commands->UpdateShaderBinding(cmd, binding, &word, sizeof(word), sizeof(uint32_t));
		word = 0xdeadbeefu;
		native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT);
		TVector<PublicationReadback> readbacks;
		const auto record = [&](VulkanDescriptorSetPtr source, const Values& expected, const char* version) -> std::string
		{
			constexpr size_t size = 2u * sizeof(Values);
			auto output = driver->CreateBuffer(size, EBufferUsageBit::StorageBuffer_Bit | EBufferUsageBit::BufferTransferSrc_Bit,
				EMemoryPropertyBit::DeviceLocal);
			auto outputBindings = driver->CreateShaderBindings();
			if (!driver->AddBufferToShaderBindings(outputBindings, output, "outputValue", 0u) ||
				!VulkanApi::IsCompatible(pipeline->m_layout, outputBindings->m_vulkan.m_descriptorSet, 1u))
				return name + ": output descriptor setup failed";
			native->BindDescriptorSet(pipeline->m_layout, { source, outputBindings->m_vulkan.m_descriptorSet }, VK_PIPELINE_BIND_POINT_COMPUTE);
			native->Dispatch(1u, 1u, 1u);
			PublicationReadback readback{ driver->CreateBuffer(size, EBufferUsageBit::BufferTransferDst_Bit, PublicationHostMemory),
				Expected({ expected, InputValues[1] }), name + " " + version };
			std::memset(readback.m_buffer->GetPointer(), 0xa7, size);
			native->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
			native->MemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			native->CopyBuffer(*output->m_vulkan.m_buffer->Get(), *readback.m_buffer->m_vulkan.m_buffer->Get(), size);
			readbacks.Emplace(std::move(readback));
			return {};
		};
		auto error = record(rawA, InputValues[0], "A after partial word update");
		if (!error.empty()) return error;
		if (AddManagedSource(inputs, name, uniform) != binding || inputs->GetDescriptorRevision() != revision + 1u ||
			static_cast<VkDescriptorSet>(*inputs->m_vulkan.m_descriptorSet) == originalNative)
			return name + ": replacement B did not preserve binding identity and publish once";
		TWeakPtr<ManagedBufferOwner> weakB(binding->m_vulkan.m_valueBinding);
		const auto rangeB = *binding->m_vulkan.m_valueBinding->Get();
		auto rawB = rawSet(rangeB);
		if (!rawB->TryCompile() || !VulkanApi::IsCompatible(pipeline->m_layout, rawB, 0u))
			return name + ": raw B descriptors did not match the shader";
		native->MemoryBarrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		Values dataB = InputValues[2];
		commands->UpdateShaderBinding(cmd, binding, dataB.data(), sizeof(dataB));
		dataB.fill(0xdeadbeefu);
		native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT);
		error = record(rawB, InputValues[2], "B");
		if (!error.empty()) return error;
		error = record(rawA, InputValues[0], "retained A");
		if (!error.empty()) return error;
		binding.Clear();
		inputs.Clear();
		blocker.Clear();
		if (!weakA.TryLock() || !weakB.TryLock())
			return name + ": bare uploads did not retain both managed destinations before submission";
		error = FinishPublication(cmd, readbacks);
		if (!error.empty()) return name + ": " + error;
		if (weakA.TryLock() || weakB.TryLock())
			return name + ": a managed destination remained owned after fence/reset while raw descriptors stayed alive";
		if (!rawA->IsCompiled() || !rawB->IsCompiled()) return name + ": raw descriptors were not retained through the expiry check";
		return {};
	}

	std::string ValidateManagedOwnerGate(ShaderSetPtr shader, bool uniform, bool projected)
	{
		auto& driver = Renderer::GetDriver();
		auto* nativeDriver = driver.DynamicCast<VulkanGraphicsDriver>();
		const std::string name = std::format("DescriptorRetentionGate_{}_{}", uniform ? "UBO" : "SSBO", projected ? "projected" : "direct");
		auto inputs = driver->CreateShaderBindings();
		VulkanComputePipelinePtr pipeline;
		if (projected)
		{
			auto neighbor = CreatePublicationBuffer(InputValues[1]);
			if (!driver->AddBufferToShaderBindings(inputs, neighbor, "neighbor", 0u) ||
				!driver->AddBufferToShaderBindings(inputs, neighbor, "unused", 31u))
				return name + ": projection input setup failed";
			pipeline = nativeDriver->GetOrAddComputePipeline(shader->GetComputeShaderRHI());
			if (!pipeline || !pipeline->IsCompiled()) return name + ": compute pipeline is unavailable";
		}
		auto binding = AddManagedSource(inputs, name, uniform);
		if (!binding || !binding->m_vulkan.m_valueBinding || !inputs->m_vulkan.m_descriptorSet ||
			!inputs->m_vulkan.m_descriptorSet->IsCompiled()) return name + ": managed A was not published";
		TWeakPtr<ManagedBufferOwner> weakA(binding->m_vulkan.m_valueBinding);
		auto directA = inputs->m_vulkan.m_descriptorSet;
		auto retainedA = directA;
		const uint64_t revision = inputs->GetDescriptorRevision();
		if (projected)
		{
			if (nativeDriver->IsCompatible(pipeline->m_layout, { inputs })[0])
				return name + ": unused binding did not force projection";
			auto sets = nativeDriver->GetCompatibleDescriptorSets(pipeline->m_layout, { inputs });
			if (sets.Num() != 1u || !sets[0] || !sets[0]->IsCompiled() || sets[0] == directA)
				return name + ": distinct projected A was not prepared";
			retainedA = sets[0];
			sets.Clear();
		}
		if (AddManagedSource(inputs, name, uniform) != binding || inputs->GetDescriptorRevision() != revision + 1u ||
			inputs->m_vulkan.m_descriptorSet == directA || !inputs->m_vulkan.m_descriptorSet->IsCompiled())
			return name + ": replacement B did not preserve binding identity and publish once";
		// Neither a saved m_vulkan value nor the direct set may mask projection ownership.
		directA.Clear();
		if (!weakA.TryLock()) return name + ": A expired while its retained native descriptor set was still alive";
		if (projected)
		{
			nativeDriver->CollectGarbage_RenderThread();
			if (!weakA.TryLock()) return name + ": projected A lost its allocation when the stale cache key was removed";
		}
		retainedA.Clear();
		if (weakA.TryLock()) return name + ": A remained owned after its last native descriptor was released";
		return {};
	}

	std::string ValidateManagedReadbacks(ShaderSetPtr shader, bool uniform, bool projected)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto* nativeDriver = driver.DynamicCast<VulkanGraphicsDriver>();
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		const std::string name = std::format("DescriptorRetentionGpu_{}_{}", uniform ? "UBO" : "SSBO", projected ? "projected" : "direct");
		auto pipeline = nativeDriver->GetOrAddComputePipeline(shader->GetComputeShaderRHI());
		if (!pipeline || !pipeline->IsCompiled() || pipeline->m_layout->m_descriptionSetLayouts.Num() != 2u)
			return name + ": compute pipeline did not expose two sets";
		auto blocker = driver->CreateShaderBindings();
		if (!AddManagedSource(blocker, name, uniform)) return name + ": live allocation blocker failed";
		auto inputs = driver->CreateShaderBindings();
		auto neighbor = CreatePublicationBuffer(InputValues[1]);
		if (!driver->AddBufferToShaderBindings(inputs, neighbor, "neighbor", 0u) ||
			(projected && !driver->AddBufferToShaderBindings(inputs, neighbor, "unused", 31u)))
			return name + ": input setup failed";
		auto binding = AddManagedSource(inputs, name, uniform);
		if (!binding || !binding->m_vulkan.m_valueBinding) return name + ": managed A allocation failed";
		const size_t alignment = uniform ? device->GetMinUboOffsetAlignment() : device->GetMinSsboOffsetAlignment();
		if (binding->GetBufferOffset() % alignment != 0u || (uniform && binding->GetBufferOffset() == 0u) ||
			(!uniform && !binding->m_vulkan.m_bBindSsboWithOffset))
			return name + ": managed source did not expose the required aligned offset";
		TWeakPtr<ManagedBufferOwner> weakA(binding->m_vulkan.m_valueBinding);
		auto nativeA = inputs->m_vulkan.m_descriptorSet;
		const VkDescriptorSet directHandleA = *nativeA;
		const uint64_t revision = inputs->GetDescriptorRevision();
		if (!projected)
		{
			auto reflectedLayout = pipeline->m_layout->m_descriptionSetLayouts[0];
			auto directLayout = nativeA->GetDescriptorSetLayout();
			const auto& reflectedBindings = reflectedLayout->m_descriptorSetLayoutBindings;
			const auto& directBindings = directLayout->m_descriptorSetLayoutBindings;
			if (directBindings.Num() != reflectedBindings.Num() ||
				directLayout->GetVariableDescriptorBinding() != reflectedLayout->GetVariableDescriptorBinding())
				return name + ": direct layout count/variable binding differs from reflection";
			for (const auto& expected : reflectedBindings)
			{
				const size_t index = directBindings.FindIf([&](const auto& actual) { return actual.binding == expected.binding; });
				if (index == size_t(-1)) return name + ": direct layout lacks a reflected binding";
				const auto& actual = directBindings[index];
				if (actual.descriptorType != expected.descriptorType || actual.descriptorCount != expected.descriptorCount ||
					actual.stageFlags != expected.stageFlags || actual.pImmutableSamplers != expected.pImmutableSamplers)
					return std::format("{}: direct binding {} differs from reflection", name, expected.binding);
			}
		}
		if (nativeDriver->IsCompatible(pipeline->m_layout, { inputs })[0] == projected)
		{
			std::string error = name + ": source did not take the requested direct/projected path";
			for (const auto& layout : { pipeline->m_layout->m_descriptionSetLayouts[0], nativeA->GetDescriptorSetLayout() })
			{
				error += " [";
				for (const auto& entry : layout->m_descriptorSetLayoutBindings)
					error += std::format(" binding={},type={},count={},stages={}", entry.binding,
						static_cast<uint32_t>(entry.descriptorType), entry.descriptorCount, entry.stageFlags);
				error += " ]";
			}
			return error;
		}
		{
			auto sets = nativeDriver->GetCompatibleDescriptorSets(pipeline->m_layout, { inputs });
			if (sets.Num() != 1u || !sets[0] || !sets[0]->IsCompiled() || (sets[0] == nativeA) == projected)
				return name + ": A did not preserve exact direct reuse or independent projection";
			nativeA = sets[0];
			sets.Clear();
		}
		auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(cmd, true);
		auto native = cmd->m_vulkan.m_commandBuffer;
		native->BindPipeline(pipeline);
		native->AddDependency(shader->GetComputeShaderRHI());
		native->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		commands->UpdateShaderBinding(cmd, binding, InputValues[0].data(), sizeof(Values));
		native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT);
		TVector<PublicationReadback> readbacks;
		const auto record = [&](VulkanDescriptorSetPtr source, const Values& expected, const char* version) -> std::string
		{
			constexpr size_t size = 2u * sizeof(Values);
			auto output = driver->CreateBuffer(size, EBufferUsageBit::StorageBuffer_Bit | EBufferUsageBit::BufferTransferSrc_Bit,
				EMemoryPropertyBit::DeviceLocal);
			auto outputBindings = driver->CreateShaderBindings();
			if (!driver->AddBufferToShaderBindings(outputBindings, output, "outputValue", 0u) ||
				!VulkanApi::IsCompatible(pipeline->m_layout, outputBindings->m_vulkan.m_descriptorSet, 1u))
				return name + ": output descriptor setup failed";
			// Bind the immutable native version, not the RHI entry that will become B.
			native->BindDescriptorSet(pipeline->m_layout, { source, outputBindings->m_vulkan.m_descriptorSet }, VK_PIPELINE_BIND_POINT_COMPUTE);
			native->Dispatch(1u, 1u, 1u);
			PublicationReadback readback{ driver->CreateBuffer(size, EBufferUsageBit::BufferTransferDst_Bit, PublicationHostMemory),
				Expected({ expected, InputValues[1] }), name + " " + version };
			std::memset(readback.m_buffer->GetPointer(), 0xa7, size);
			native->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
			native->MemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			native->CopyBuffer(*output->m_vulkan.m_buffer->Get(), *readback.m_buffer->m_vulkan.m_buffer->Get(), size);
			readbacks.Emplace(std::move(readback));
			return {};
		};
		auto error = record(nativeA, InputValues[0], "A");
		if (!error.empty()) return error;
		if (AddManagedSource(inputs, name, uniform) != binding || inputs->GetDescriptorRevision() != revision + 1u ||
			static_cast<VkDescriptorSet>(*inputs->m_vulkan.m_descriptorSet) == directHandleA)
			return name + ": replacement B did not preserve binding identity and publish once";
		auto nativeB = inputs->m_vulkan.m_descriptorSet;
		if (nativeDriver->IsCompatible(pipeline->m_layout, { inputs })[0] == projected)
			return name + ": B did not keep the requested direct/projected path";
		{
			auto sets = nativeDriver->GetCompatibleDescriptorSets(pipeline->m_layout, { inputs });
			if (sets.Num() != 1u || !sets[0] || !sets[0]->IsCompiled() || (sets[0] == nativeB) == projected || sets[0] == nativeA)
				return name + ": B did not preserve exact direct reuse or independent projection";
			nativeB = sets[0];
			sets.Clear();
		}
		native->MemoryBarrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		commands->UpdateShaderBinding(cmd, binding, InputValues[2].data(), sizeof(Values));
		native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT);
		error = record(nativeB, InputValues[2], "B");
		if (!error.empty()) return error;
		error = record(nativeA, InputValues[0], "retained A");
		if (!error.empty()) return error;
		nativeA.Clear();
		nativeB.Clear();
		binding.Clear();
		inputs.Clear();
		blocker.Clear();
		neighbor.Clear();
		nativeDriver->CollectGarbage_RenderThread();
		if (!weakA.TryLock()) return name + ": recorded native descriptors did not retain A before submission";
		error = FinishPublication(cmd, readbacks);
		if (!error.empty()) return name + ": " + error;
		nativeDriver->CollectGarbage_RenderThread();
		if (weakA.TryLock()) return name + ": A remained owned after the fence, command reset and cache collection";
		return {};
	}

	bool PublishedHashIsCurrent(RHIShaderBindingSetPtr bindings)
	{
		const size_t published = bindings->GetCompatibilityHashCode();
		bindings->RecalculateCompatibility();
		return published == bindings->GetCompatibilityHashCode();
	}

	std::string ValidateImagePublication(ShaderSetPtr shader, bool storage)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto valueA = CreatePublicationTexture(InputValues[0]);
		auto neighbor = CreatePublicationTexture(InputValues[1]);
		auto valueC = CreatePublicationTexture(InputValues[2]);
		auto neighborBuffer = CreatePublicationBuffer(InputValues[1]);
		const auto add = [&](RHIShaderBindingSetPtr& set, const char* name, const TVector<RHITexturePtr>& textures, uint32_t index)
		{
			return storage ? driver->AddStorageImageToShaderBindings(set, name, textures, index) :
				driver->AddSamplerToShaderBindings(set, name, textures, index);
		};
		auto bindings = driver->CreateShaderBindings();
		if (!driver->AddBufferToShaderBindings(bindings, neighborBuffer, "neighbor", 0u)) return "image neighbor binding failed";
		auto original = add(bindings, "source", { valueA, neighbor }, 1u);
		if (!original) return "initial image binding failed";
		bindings->RecalculateCompatibility();
		const PublishedState before(bindings);
		auto unavailable = RHITexturePtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false);
		unavailable->m_vulkan.m_image = valueA->m_vulkan.m_image;
		unavailable->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, unavailable->m_vulkan.m_image);
		// Native75 rejects this nonnull wrapper safely, but old Add* returns success after changing CPU state.
		const auto failedAdd = add(bindings, "source", { valueA, neighbor, unavailable }, 1u);
		const bool stateUnchanged = before.Unchanged();
		if (failedAdd || !stateUnchanged)
			return std::format("failed image Add: returned binding={}, state unchanged={} (expected false/true)",
				static_cast<bool>(failedAdd), stateUnchanged);
		for (uint32_t missingView = 0u; missingView < 2u; ++missingView)
		{
			if (missingView != 0u) unavailable->m_vulkan.m_imageView.Clear();
			if (add(bindings, "source", { unavailable }, 1u) || !before.Unchanged())
				return "unavailable image replacement changed the published state";
			if (add(bindings, "failedSource", { unavailable }, 2u) || !before.Unchanged())
				return "unavailable image addition left a new-name placeholder";
		}
		auto badCopy = RHIShaderBindingPtr::Make();
		badCopy->m_vulkan = original->m_vulkan;
		badCopy->SetLayout(original->GetLayout());
		badCopy->SetTextureBindings({ unavailable, neighbor });
		if (driver->AddShaderBinding(bindings, badCopy, "source", 1u) || !before.Unchanged() ||
			driver->AddShaderBinding(bindings, badCopy, "failedCopy", 2u) || !before.Unchanged())
			return "copying an unavailable texture binding changed the destination";
		if (!bindings->NeedsStorageBuffer()) return "image publication lost its buffer neighbor's storage requirement";

		auto donor = driver->CreateShaderBindings();
		auto donorBinding = add(donor, "donor", { valueA, neighbor }, 4u);
		if (!donorBinding) return "image copy donor could not be prepared";
		const PublishedState donorState(donor);
		auto copy = driver->CreateShaderBindings();
		if (!driver->AddBufferToShaderBindings(copy, neighborBuffer, "neighbor", 0u)) return "copy neighbor binding failed";
		const uint64_t copyRevision = copy->GetDescriptorRevision();
		auto copied = driver->AddShaderBinding(copy, donorBinding, "copiedSource", 1u);
		if (!copied || copied == donorBinding || copied->GetLayout().m_name != "copiedSource" ||
			copied->GetLayout().m_binding != 1u || copied->GetLayout().m_arrayCount != 2u ||
			copied->m_vulkan.m_descriptorSetLayout.binding != 1u || copied->m_vulkan.m_descriptorSetLayout.descriptorCount != 2u ||
			copied->GetTextureBindings() != donorBinding->GetTextureBindings() || !donorState.Unchanged() ||
			copy->GetDescriptorRevision() != copyRevision + 1u || !PublishedHashIsCurrent(copy))
			return "AddShaderBinding did not copy/remap the complete image array";

		// A separate CPU fixture asks the compatible builder to select two entries from a native array of three.
		// Never mutate A's textures or views, and never dispatch the unavailable selected entry.
		auto projection = driver->CreateShaderBindings();
		if (!driver->AddBufferToShaderBindings(projection, neighborBuffer, "neighbor", 0u)) return "projection neighbor failed";
		auto projectionSource = add(projection, "source", { valueA, neighbor, valueC }, 1u);
		if (!projectionSource) return "projection source preparation failed";
		projectionSource->SetTextureBindings({ unavailable, neighbor, valueC });
		auto* nativeDriver = driver.DynamicCast<VulkanGraphicsDriver>();
		auto pipeline = nativeDriver->GetOrAddComputePipeline(shader->GetComputeShaderRHI());
		if (!pipeline || !pipeline->IsCompiled() || nativeDriver->IsCompatible(pipeline->m_layout, { projection })[0])
			return "selected unavailable image did not require a compatible cache miss";
		const PublishedState projectionState(projection);
		if (!nativeDriver->GetCompatibleDescriptorSets(pipeline->m_layout, { projection }).IsEmpty() ||
			!projectionState.Unchanged() || !before.Unchanged()) return "failed compatible projection changed a published set";
		projectionSource->SetTextureBindings({ valueA, neighbor, unavailable });
		auto projected = nativeDriver->GetCompatibleDescriptorSets(pipeline->m_layout, { projection });
		if (projected.Num() != 1u || !projected[0] || !projected[0]->IsCompiled() ||
			projected[0] == projection->m_vulkan.m_descriptorSet || !before.Unchanged())
			return "corrected compatible projection did not ignore the unused unavailable array tail";

		auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(cmd, true);
		cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		for (auto texture : { valueA, neighbor, valueC })
		{
			if (storage) commands->ImageMemoryBarrier(cmd, texture, EImageLayout::ComputeRead);
			else commands->ImageMemoryBarrierForComputeSampling(cmd, texture);
		}
		TVector<PublicationReadback> readbacks;
		auto error = RecordPublication(cmd, shader, bindings, Expected({ InputValues[0], InputValues[1], InputValues[1] }),
			storage ? "storage A after refusal" : "sampler A after refusal", readbacks);
		if (!error.empty()) return error;
		error = RecordPublication(cmd, shader, copy, Expected({ InputValues[0], InputValues[1], InputValues[1] }),
			storage ? "copied storage array" : "copied sampler array", readbacks);
		if (!error.empty()) return error;
		error = RecordPublication(cmd, shader, projection, Expected({ InputValues[0], InputValues[1], InputValues[1] }),
			storage ? "corrected storage projection" : "corrected sampler projection", readbacks);
		if (!error.empty()) return error;
		// Successful mutation is allowed to change this binding object. A is already recorded above.
		for (uint32_t count : { 1u, 2u })
		{
			const uint64_t revision = bindings->GetDescriptorRevision();
			auto oldNative = bindings->m_vulkan.m_descriptorSet;
			TVector<RHITexturePtr> textures{ valueC };
			if (count == 2u) textures.Add(neighbor);
			if (add(bindings, "source", textures, 1u) != original || bindings->GetDescriptorRevision() != revision + 1u ||
				bindings->m_vulkan.m_descriptorSet == oldNative || original->GetTextureBindings() != textures ||
				original->m_vulkan.m_descriptorSetLayout.descriptorCount != count || !PublishedHashIsCurrent(bindings))
				return "successful image replacement did not publish exactly once into the existing binding";
			size_t matches = 0u;
			for (const auto& layout : bindings->GetLayoutBindings())
				if (layout.m_name == "source") { ++matches; if (layout.m_arrayCount != count) return "stale fixed-array layout"; }
			if (matches != 1u) return "fixed-array replacement duplicated the layout name";
		}
		error = RecordPublication(cmd, shader, bindings, Expected({ InputValues[2], InputValues[1], InputValues[1] }),
			storage ? "storage C" : "sampler C", readbacks);
		if (!error.empty()) return error;
		bindings.Clear();
		copy.Clear();
		return FinishPublication(cmd, readbacks);
	}

	std::string ValidateBufferPublication(ShaderSetPtr storageShader, ShaderSetPtr uniformShader)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto valueA = CreatePublicationBuffer(InputValues[0]);
		auto neighbor = CreatePublicationBuffer(InputValues[1]);
		auto valueC = CreatePublicationBuffer(InputValues[2]);
		auto bindings = driver->CreateShaderBindings();
		if (!driver->AddBufferToShaderBindings(bindings, neighbor, "neighbor", 0u)) return "buffer neighbor binding failed";
		auto original = driver->AddBufferToShaderBindings(bindings, valueA, "source", 1u);
		if (!original) return "initial external buffer binding failed";
		const PublishedState before(bindings);
		auto unavailable = RHIBufferPtr::Make(EBufferUsageBit::StorageBuffer_Bit, EMemoryPropertyBit::DeviceLocal);
		auto uncompiled = VulkanBufferPtr::Make(device, sizeof(Values), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE);
		unavailable->m_vulkan.m_buffer = TManagedMemoryPtr<VulkanBufferMemoryPtr, VulkanBufferAllocator>::Make(
			TMemoryPtr<VulkanBufferMemoryPtr>(0u, 0u, sizeof(Values), VulkanBufferMemoryPtr(uncompiled, 0u, sizeof(Values)), UINT32_MAX),
			TWeakPtr<VulkanBufferAllocator>{});
		if (driver->AddBufferToShaderBindings(bindings, unavailable, "source", 1u) || !before.Unchanged() ||
			driver->AddBufferToShaderBindings(bindings, unavailable, "failedSource", 2u) || !before.Unchanged())
			return "unavailable external buffer changed published state or left a placeholder";

		auto donor = driver->CreateShaderBindings();
		auto donorBinding = driver->AddBufferToShaderBindings(donor, valueC, "donor", 4u);
		if (!donorBinding) return "buffer copy donor could not be prepared";
		const PublishedState donorState(donor);
		auto copy = driver->CreateShaderBindings();
		if (!driver->AddBufferToShaderBindings(copy, neighbor, "neighbor", 0u)) return "buffer copy neighbor failed";
		const uint64_t copyRevision = copy->GetDescriptorRevision();
		auto copied = driver->AddShaderBinding(copy, donorBinding, "copiedSource", 1u);
		if (!copied || copied == donorBinding || copied->GetLayout().m_name != "copiedSource" ||
			copied->GetLayout().m_binding != 1u || copied->m_vulkan.m_descriptorSetLayout.binding != 1u ||
			copied->m_vulkan.m_valueBinding != donorBinding->m_vulkan.m_valueBinding ||
			copied->m_vulkan.m_bBindSsboWithOffset != donorBinding->m_vulkan.m_bBindSsboWithOffset ||
			copied->GetStorageInstanceIndex() != donorBinding->GetStorageInstanceIndex() || !donorState.Unchanged() ||
			copy->GetDescriptorRevision() != copyRevision + 1u || !PublishedHashIsCurrent(copy))
			return "AddShaderBinding did not retain/remap the shared buffer payload";

		// These managed allocations remain owned until the fence. Only ordinary buffers are replaced after recording A.
		auto uniformSet = driver->CreateShaderBindings();
		const uint64_t uniformRevision = uniformSet->GetDescriptorRevision();
		auto uniform = driver->AddBufferToShaderBindings(uniformSet, "uniformSource", sizeof(Values), 1u, EShaderBindingType::UniformBuffer);
		if (!uniform || uniformSet->NeedsStorageBuffer() || uniformSet->GetDescriptorRevision() != uniformRevision + 1u ||
			!uniform->m_vulkan.m_valueBinding || !PublishedHashIsCurrent(uniformSet)) return "allocating UBO publication failed";
		const PublishedState uniformState(uniformSet);
		if (driver->AddSsboToShaderBindings(uniformSet, "uniformSource", sizeof(Values), 1u, 1u, true) || !uniformState.Unchanged())
			return "incompatible SSBO replacement changed a managed UBO owner";
		if (!driver->AddBufferToShaderBindings(uniformSet, neighbor, "neighbor", 0u)) return "UBO neighbor binding failed";

		auto allocatedSet = driver->CreateShaderBindings();
		const uint64_t allocatedRevision = allocatedSet->GetDescriptorRevision();
		const size_t rangeSize = (std::max)(sizeof(Values), static_cast<size_t>(device->GetMinSsboOffsetAlignment()));
		auto allocated = driver->AddBufferToShaderBindings(allocatedSet, "allocatedSource", rangeSize, 1u, EShaderBindingType::StorageBuffer);
		if (!allocated || !allocatedSet->NeedsStorageBuffer() || allocatedSet->GetDescriptorRevision() != allocatedRevision + 1u ||
			!allocated->m_vulkan.m_valueBinding || !PublishedHashIsCurrent(allocatedSet)) return "allocating SSBO publication failed";
		const PublishedState allocatedState(allocatedSet);
		if (driver->AddBufferToShaderBindings(allocatedSet, "allocatedSource", sizeof(Values), 1u, EShaderBindingType::UniformBuffer) ||
			!allocatedState.Unchanged()) return "incompatible UBO replacement changed a managed SSBO owner";
		// The ordinary allocator aligns to size, so request a range that can legally be bound with an explicit offset.
		if (allocated->GetBufferOffset() % device->GetMinSsboOffsetAlignment() != 0u)
			return "allocated SSBO inspection range is not aligned for an offset descriptor";
		// Read this exact range, not the generic allocator's separate instance-index/base-range contract.
		auto rangeDonor = RHIShaderBindingPtr::Make();
		rangeDonor->SetLayout(allocated->GetLayout());
		rangeDonor->m_vulkan = allocated->m_vulkan;
		rangeDonor->m_vulkan.m_bBindSsboWithOffset = true;
		auto rangeSet = driver->CreateShaderBindings();
		if (!driver->AddShaderBinding(rangeSet, rangeDonor, "source", 1u) ||
			!driver->AddBufferToShaderBindings(rangeSet, neighbor, "neighbor", 0u) || !allocatedState.Unchanged())
			return "offset-bound inspection of an allocated SSBO changed its original binding";

		auto reflected = driver->CreateShaderBindings();
		if (!driver->FillShadersLayout(reflected, { storageShader->GetDebugComputeShaderRHI() }, 0u) ||
			!reflected->GetShaderBindings().IsEmpty()) return "debug reflection did not prepare metadata-only bindings";
		const auto& layouts = reflected->GetLayoutBindings();
		const size_t sourceIndex = layouts.FindIf([](const auto& layout) { return layout.m_binding == 1u; });
		if (sourceIndex == size_t(-1)) return "debug reflection did not expose the source buffer";
		const auto reflectedSource = layouts[sourceIndex];
		if (reflectedSource.m_name.empty() || reflectedSource.m_members.IsEmpty() ||
			reflectedSource.m_type != EShaderBindingType::StorageBuffer || reflectedSource.m_name == "aliasedSource")
			return "source reflection did not retain member names/type";
		const std::string member = reflectedSource.m_members[0].m_name;
		if (!reflected->HasBinding(reflectedSource.m_name) || !reflected->HasParameter(reflectedSource.m_name + "." + member))
			return "reflected source metadata was not queryable before aliasing";
		const uint64_t reflectedRevision = reflected->GetDescriptorRevision();
		auto ssbo = driver->AddSsboToShaderBindings(reflected, "aliasedSource", sizeof(Values), 1u, 1u, true);
		auto renamedLayout = reflectedSource;
		renamedLayout.m_name = "aliasedSource";
		if (!ssbo || ssbo->GetLayout() != renamedLayout || !ssbo->m_vulkan.m_bBindSsboWithOffset ||
			reflected->HasBinding(reflectedSource.m_name) || reflected->HasParameter(reflectedSource.m_name + "." + member) ||
			!reflected->HasBinding("aliasedSource") || !reflected->HasParameter("aliasedSource." + member) ||
			reflected->GetDescriptorRevision() != reflectedRevision + 1u || !PublishedHashIsCurrent(reflected))
			return "AddSsbo did not replace the reflected name while preserving its members/type";
		size_t slotCount = 0u;
		for (const auto& layout : reflected->GetLayoutBindings()) if (layout.m_binding == 1u) ++slotCount;
		if (slotCount != 1u) return "reflected alias left duplicate metadata at binding 1";
		if (!driver->AddBufferToShaderBindings(reflected, neighbor, "neighbor", 0u)) return "reflected neighbor binding failed";
		const PublishedState reflectedState(reflected);
		auto wrongType = CreatePublicationBuffer(InputValues[2], EBufferUsageBit::UniformBuffer_Bit);
		if (driver->AddBufferToShaderBindings(reflected, wrongType, "aliasedSource", 1u) || !reflectedState.Unchanged() ||
			driver->AddBufferToShaderBindings(reflected, "aliasedSource", sizeof(Values), 1u, EShaderBindingType::UniformBuffer) ||
			!reflectedState.Unchanged()) return "incompatible buffer request rewrote reflected type or managed owner";

		auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(cmd, true);
		cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		TVector<PublicationReadback> readbacks;
		auto error = RecordPublication(cmd, storageShader, bindings, Expected({ InputValues[0], InputValues[1] }),
			"buffer A after refusal", readbacks);
		if (!error.empty()) return error;
		const uint64_t revision = bindings->GetDescriptorRevision();
		auto oldNative = bindings->m_vulkan.m_descriptorSet;
		if (driver->AddBufferToShaderBindings(bindings, valueC, "source", 1u) != original ||
			bindings->GetDescriptorRevision() != revision + 1u || bindings->m_vulkan.m_descriptorSet == oldNative ||
			!PublishedHashIsCurrent(bindings)) return "corrected external buffer did not publish once into the existing binding";
		error = RecordPublication(cmd, storageShader, bindings, Expected({ InputValues[2], InputValues[1] }), "buffer C", readbacks);
		if (!error.empty()) return error;
		error = RecordPublication(cmd, storageShader, copy, Expected({ InputValues[2], InputValues[1] }), "copied buffer C", readbacks);
		if (!error.empty()) return error;
		commands->UpdateShaderBinding(cmd, uniform, InputValues[0].data(), sizeof(Values));
		commands->UpdateShaderBinding(cmd, allocated, InputValues[2].data(), sizeof(Values));
		commands->UpdateShaderBinding(cmd, ssbo, InputValues[0].data(), sizeof(Values));
		cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT);
		error = RecordPublication(cmd, uniformShader, uniformSet, Expected({ InputValues[0], InputValues[1] }), "allocated UBO", readbacks);
		if (!error.empty()) return error;
		error = RecordPublication(cmd, storageShader, rangeSet, Expected({ InputValues[2], InputValues[1] }), "allocated SSBO range", readbacks);
		if (!error.empty()) return error;
		error = RecordPublication(cmd, storageShader, reflected, Expected({ InputValues[0], InputValues[1] }), "reflected AddSsbo alias", readbacks);
		if (!error.empty()) return error;
		return FinishPublication(cmd, readbacks);
	}

	std::string ValidateVariablePublication(ShaderSetPtr shader, bool supported, bool& outSparseTested)
	{
		if (!supported) return {};
		auto& driver = Renderer::GetDriver();
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto valueA = CreatePublicationTexture(InputValues[0]);
		auto valueC = CreatePublicationTexture(InputValues[2]);
		auto bindings = driver->CreateShaderBindings();
		auto original = driver->AddSamplerToShaderBindings(bindings, "source", valueA, 1u, true, 4u);
		if (!original || bindings->GetVariableDescriptorCount() != 4u) return "initial variable sampler capacity was not four";
		const PublishedState before(bindings);
		TVector<RHITexturePtr> tooMany{ valueA, valueA, valueA, valueA, valueC };
		if (driver->AddSamplerToShaderBindings(bindings, "another", valueA, 2u, true, 4u) || !before.Unchanged() ||
			driver->AddSamplerToShaderBindings(bindings, "source", valueA, 1u, false) || !before.Unchanged() ||
			driver->AddSamplerToShaderBindings(bindings, "source", tooMany, 1u, true, 8u) || !before.Unchanged() ||
			driver->AddShaderBinding(bindings, original, "another", 2u) || !before.Unchanged())
			return "variable name/flag/capacity refusal changed the published set";
		auto fixedDonor = RHIShaderBindingPtr::Make();
		fixedDonor->m_vulkan = original->m_vulkan;
		fixedDonor->SetTextureBindings(original->GetTextureBindings());
		auto fixedLayout = original->GetLayout();
		fixedLayout.m_bVariableDescriptorCount = false;
		fixedDonor->SetLayout(fixedLayout);
		if (driver->AddShaderBinding(bindings, fixedDonor, "source", 1u) || !before.Unchanged())
			return "AddShaderBinding bypassed the published variable flag";

		const uint64_t revision = bindings->GetDescriptorRevision();
		auto oldNative = bindings->m_vulkan.m_descriptorSet;
		const TVector<RHITexturePtr> empty;
		if (driver->AddSamplerToShaderBindings(bindings, "source", empty, 1u, true, 1u) != original ||
			bindings->GetDescriptorRevision() != revision + 1u || bindings->GetVariableDescriptorCount() != 4u ||
			!original->GetTextureBindings().IsEmpty() || original->GetLayout().m_arrayCount != 4u ||
			original->m_vulkan.m_descriptorSetLayout.descriptorCount != 4u || !PublishedHashIsCurrent(bindings))
			return "empty variable publication did not retain its capacity and binding identity";
		auto emptyNative = bindings->m_vulkan.m_descriptorSet;
		auto emptyLayout = emptyNative->GetDescriptorSetLayout();
		if (emptyNative == oldNative || !emptyNative->IsCompiled() || emptyNative->GetVariableDescriptorCount() != 4u ||
			!emptyNative->m_descriptors.IsEmpty() || !emptyLayout->HasVariableDescriptorBinding() ||
			emptyLayout->GetVariableDescriptorBinding() != 1 || emptyLayout->m_descriptorSetLayoutBindings.Num() != 1u ||
			emptyLayout->m_descriptorSetLayoutBindings[0].binding != 1u ||
			emptyLayout->m_descriptorSetLayoutBindings[0].descriptorCount != 4u)
			return "empty variable set lost its allocated count or native layout";
		if (!device->IsDescriptorUpdateAfterBindSupported()) return {};
		if (driver->AddSamplerToShaderBindings(bindings, "source", valueA, 1u, true, 4u) != original)
			return "restoring a populated variable sampler failed";
		// A variable descriptor binding must remain the highest binding; the unused extra is at zero.
		auto neighbor = CreatePublicationBuffer(InputValues[1]);
		if (!driver->AddBufferToShaderBindings(bindings, neighbor, "unused", 0u)) return "sparse extra buffer failed";
		const PublishedState beforeAppend(bindings);
		auto unavailable = RHITexturePtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false);
		unavailable->m_vulkan.m_image = valueA->m_vulkan.m_image;
		driver->UpdateShaderBinding(bindings, "source", unavailable, 3u);
		if (!beforeAppend.Unchanged()) return "indexed missing-view update changed the published variable set";
		driver->UpdateShaderBinding(bindings, "source", valueC, 3u);
		const auto& textures = original->GetTextureBindings();
		if (bindings->GetDescriptorRevision() != beforeAppend.m_revision + 1u ||
			bindings->m_vulkan.m_descriptorSet != beforeAppend.m_native || bindings->GetVariableDescriptorCount() != 4u ||
			textures.Num() != 4u || textures[0] != valueA || textures[1] || textures[2] || textures[3] != valueC ||
			!bindings->m_vulkan.m_descriptorSet->ReferencesImageView(1u, 3u, valueC->m_vulkan.m_imageView))
			return "out-of-order indexed append did not retain A with two partially-bound interior holes";
		const uint64_t appendRevision = bindings->GetDescriptorRevision();
		auto appendedNative = bindings->m_vulkan.m_descriptorSet;
		if (!driver->AddBufferToShaderBindings(bindings, neighbor, "unused", 0u) ||
			bindings->GetDescriptorRevision() != appendRevision + 1u || bindings->m_vulkan.m_descriptorSet == appendedNative ||
			bindings->GetVariableDescriptorCount() != 4u || !PublishedHashIsCurrent(bindings))
			return "full rebuild did not preserve a sparse variable array";
		auto commands = Renderer::GetDriverCommands();
		auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(cmd, true);
		commands->ImageMemoryBarrierForComputeSampling(cmd, valueA);
		commands->ImageMemoryBarrierForComputeSampling(cmd, valueC);
		TVector<PublicationReadback> readbacks;
		auto error = RecordPublication(cmd, shader, bindings, Expected({ InputValues[0], InputValues[2] }),
			"sparse variable projection slots 0/3", readbacks, true);
		if (!error.empty()) return error;
		error = FinishPublication(cmd, readbacks);
		if (error.empty()) outSparseTested = true;
		return error;
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
		AddJournalEvent("DescriptorUpdateTimings", result.m_updateTimings);
		AddJournalEvent("DescriptorLayoutOrder",
			"Opposite input orders compare equally; direct UBO/SSBO A/B/A uses the original reflected pipeline and exact native sets; extra-binding sets still require projection");
		AddJournalEvent("ManagedUploadRetention",
			"Bare UBO/offset-SSBO uploads retain replaced destinations until reset, destruction or last-command release; 2 x A/B/A full 8-word readbacks passed with poisoned CPU data and aligned byte-offset updates; weak owners expired while raw descriptors stayed alive");
		AddJournalEvent("DescriptorManagedRetention",
			"Direct/projected UBO and offset-bound AddSsbo retain A across replacement B: factory-only weak gates, 4 x A/B/A full 8-word GPU readbacks, stale-cache collection and weak expiry after fence/reset passed");
		AddJournalEvent("DescriptorPreparationEvidence",
			"All-or-none native preparation, buffer/image handle rejection, same-candidate retry, empty/sparse sets and 4 x 8-word GPU readbacks (A/C/A and appended set, each with neighbor) passed");
		AddJournalEvent("DescriptorVariableCapacity", result.m_bVariableDescriptorsTested ?
			"Layout capacity 4, allocation capacity 2: rejected elements 2/3, sparse retry, valid append and reversed mixed fixed/variable layout passed" :
			"Skipped: device lacks variable-count, partially-bound or update-unused descriptor support");
		AddJournalEvent("DescriptorPublicationEvidence",
			"Failed replace/new-name/copy preserved CPU/native state; sampler/storage/buffer A before C, copied arrays/buffers and corrected compatible projections passed full GPU readback with unchanged neighbors");
		AddJournalEvent("DescriptorBufferPublication",
			"Allocated UBO, AddSsbo and reflected alias readbacks passed; generic SSBO owner/rejection/Update bytes checked through a separate offset-bound range, not its instance-index/base-range contract");
		AddJournalEvent("DescriptorVariablePublication", result.m_bVariableDescriptorsTested ?
			"Variable name/flag/capacity refusal and empty-array native capacity 4 passed" : "Skipped: variable descriptor features unavailable");
		AddJournalEvent("DescriptorSparsePublication", result.m_bSparsePublicationTested ?
			"Indexed slot 3 append, full rebuild and compiled variable projection read populated slots 0/3 with interior holes" :
			"Skipped: variable descriptors or update-after-bind support unavailable");
		MarkPassed();
		return;
	}
	if (!m_shader)
	{
		auto* registry = App::GetSubmodule<AssetRegistry>();
		if (auto info = registry->GetAssetInfoPtr("Tests/Shaders/DescriptorPreparation.shader"))
			App::GetSubmodule<ShaderCompiler>()->LoadShader(info->GetFileId(), m_shader);
	}
	const std::array<TVector<std::string>, 5> defines{
		TVector<std::string>{}, { "SAMPLED_INPUT" }, { "STORAGE_INPUT" }, { "SPARSE_INPUT" }, { "UNIFORM_INPUT" }
	};
	bool publicationReady = true;
	for (size_t i = 0u; i < m_publicationShaders.size(); ++i)
	{
		if (!m_publicationShaders[i])
			if (auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Tests/Shaders/DescriptorPublication.shader"))
				App::GetSubmodule<ShaderCompiler>()->LoadShader(info->GetFileId(), m_publicationShaders[i], defines[i]);
		publicationReady &= m_publicationShaders[i] && m_publicationShaders[i]->IsReady();
	}
	if (m_shader && m_shader->IsReady() && publicationReady)
	{
		m_validation = Tasks::CreateTaskWithResult<ValidationResult>("Native descriptor preparation validation",
			[shader = m_shader, publication = m_publicationShaders]()
			{
				ValidationResult result;
				result.m_error = ValidateLayoutOrder();
				if (!result.m_error.empty()) return result;
				// The native command layout changed: old runtimes must stop here without inline native access or GPU submission.
				for (bool uniform : { true, false })
					for (UploadRelease release : { UploadRelease::Reset, UploadRelease::Destroy, UploadRelease::TwoCommands })
					{
						result.m_error = ValidateBareUploadOwners(uniform, release);
						if (!result.m_error.empty()) return result;
					}
				// A final-header executable on old runtimes must stop before any test-side native descriptor construction or submission.
				for (bool projected : { false, true })
					for (bool uniform : { true, false })
					{
						result.m_error = ValidateManagedOwnerGate(publication[uniform ? 4u : 0u], uniform, projected);
						if (!result.m_error.empty()) return result;
					}
				for (bool uniform : { true, false })
				{
					result.m_error = ValidateBareUploadReadbacks(publication[uniform ? 4u : 0u], uniform);
					if (!result.m_error.empty()) return result;
				}
				for (bool projected : { false, true })
					for (bool uniform : { true, false })
					{
						result.m_error = ValidateManagedReadbacks(publication[uniform ? 4u : 0u], uniform, projected);
						if (!result.m_error.empty()) return result;
					}
				result.m_error = ValidateDescriptors(shader, result.m_bVariableDescriptorsTested);
				if (result.m_error.empty()) result.m_error = ValidateImagePublication(publication[1], false);
				if (result.m_error.empty()) result.m_error = ValidateImagePublication(publication[2], true);
				if (result.m_error.empty()) result.m_error = ValidateBufferPublication(publication[0], publication[4]);
				if (result.m_error.empty()) result.m_error = ValidateVariablePublication(publication[3],
					result.m_bVariableDescriptorsTested, result.m_bSparsePublicationTested);
				if (result.m_error.empty()) result.m_error = MeasureBindingUpdates(result.m_updateTimings);
				return result;
			}, EThreadType::Render);
		m_validation->Run();
	}
	else if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 30000)
	{
		MarkFailed("descriptor validation shader did not become ready within 30 seconds");
	}
}
