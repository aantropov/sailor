#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>
#include <vulkan/vulkan.h>

namespace Sailor::Tests
{
	struct VulkanBufferWrite
	{
		VkBuffer m_buffer;
		VkDeviceSize m_offset;
		VkDeviceSize m_size;
	};

	// Observe the calling thread's real transfer commands; Vulkan still executes them.
	std::vector<VulkanBufferWrite> CaptureVulkanBufferWrites(const std::function<void()>& record);

	struct VulkanComputeInputEvent
	{
		enum class Kind { Copy, Barrier, Dispatch };
		Kind m_kind;
		VkCommandBuffer m_command;
		VkBuffer m_buffer = VK_NULL_HANDLE;
		VkDeviceSize m_offset = 0, m_size = 0;
		VkPipelineStageFlags m_sourceStage = 0, m_destinationStage = 0;
		VkAccessFlags m_sourceAccess = 0, m_destinationAccess = 0;
	};

	std::vector<VulkanComputeInputEvent> CaptureVulkanComputeInputs(const std::function<void()>& record);

	struct VulkanDescriptorAllocationFailure
	{
		uint32_t m_numAllocations = 0;
		VkDescriptorSet m_firstSet = VK_NULL_HANDLE;
		VkDescriptorSetLayout m_failedLayout = VK_NULL_HANDLE;
		bool m_bFirstStorageWritten = false;
	};

	VulkanDescriptorAllocationFailure RefuseSecondVulkanDescriptorAllocation(const std::function<void()>& record);

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
		std::atomic<uint32_t> bufferDestroyCalls{ 0 };
		std::atomic<uint32_t> imageCreateCalls{ 0 };
		std::atomic<uint32_t> imageDestroyCalls{ 0 };
		std::atomic<uint32_t> fenceCreateCalls{ 0 };
		std::atomic<uint32_t> fenceDestroyCalls{ 0 };
		std::atomic<uint32_t> lastUploadLayers{ 0 };
		std::atomic<PFN_vkQueueSubmit> queueSubmit{ nullptr };
		std::atomic<PFN_vkWaitForFences> waitForFences{ nullptr };
		std::atomic<PFN_vkGetFenceStatus> getFenceStatus{ nullptr };
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
