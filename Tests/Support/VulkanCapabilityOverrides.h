#pragma once
#include <atomic>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace Sailor::Tests
{
	enum class MissingVulkanFeature
	{
		None, Anisotropy, FirstInstance, IndependentBlend, RuntimeArray,
		SampledImageIndexing, VariableDescriptorCount, PartiallyBound, DynamicRendering
	};

	enum class ValidationLayerInventory
	{
		Native, None, Primary, Compatibility, Both
	};

	struct VulkanCapabilityOverrides
	{
		std::atomic<uint32_t> missingRenderingCommands{ 0 };
		std::atomic<uint32_t> hiddenRenderingCommands{ 0 };
		std::atomic<uint32_t> deviceApiVersion{ 0 };
		std::atomic<uint32_t> loaderApiVersion{ 0 };
		std::atomic<MissingVulkanFeature> missingFeature{ MissingVulkanFeature::None };
		std::atomic<VkResult> deviceCreationResult{ VK_SUCCESS };
		std::atomic<uint32_t> featureQueries{ 0 };
		std::atomic<uint32_t> deviceCreateCalls{ 0 };
		std::atomic<uint32_t> deviceDestroyCalls{ 0 };
		std::atomic<uint32_t> instanceCreateCalls{ 0 };
		std::atomic<uint32_t> instanceDestroyCalls{ 0 };
		std::atomic<uint32_t> surfaceDestroyCalls{ 0 };
		std::atomic<uint32_t> samplerCreateCalls{ 0 };
		std::atomic<uint32_t> bufferCreateCalls{ 0 };
		std::atomic<uint32_t> coreRenderingLookups{ 0 };
		std::atomic<uint32_t> khrRenderingLookups{ 0 };
		std::atomic<uint32_t> instanceTarget{ 0 };
		std::atomic<bool> enabledKhrRendering{ false };
		std::atomic<ValidationLayerInventory> validationLayers{ ValidationLayerInventory::Native };
		std::atomic<uint32_t> layerEnumerationCalls{ 0 };
		std::atomic<uint32_t> requestedLayerCount{ 0 };
		std::atomic<bool> requestedPrimaryValidation{ false };
		std::atomic<bool> requestedCompatibilityLayer{ false };
		std::atomic<bool> requestedDebugMessenger{ false };
	};

	VulkanCapabilityOverrides& GetVulkanCapabilityOverrides();
}
