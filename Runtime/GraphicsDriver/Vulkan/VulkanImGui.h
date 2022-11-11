#pragma once
#include "imgui.h"
#include "VulkanApi.h"
#include "VulkanDevice.h"

namespace Sailor::GraphicsDriver::Vulkan
{
	struct ImGui_InitInfo
	{
		VkInstance                      Instance;
		VulkanDevicePtr                 Device;
		VkDescriptorPool                DescriptorPool;
		uint32_t                        Subpass;
		uint32_t                        MinImageCount;          // >= 2
		uint32_t                        ImageCount;             // >= MinImageCount
		VkSampleCountFlagBits           MSAASamples;            // >= VK_SAMPLE_COUNT_1_BIT (0 -> default to VK_SAMPLE_COUNT_1_BIT)
		const VkAllocationCallbacks* Allocator;
		void                            (*CheckVkResultFn)(VkResult err);
	};

	SAILOR_API bool         ImGui_Init(ImGui_InitInfo* info, VkRenderPass render_pass);
	SAILOR_API void         ImGui_Shutdown();
	SAILOR_API void         ImGui_NewFrame();
	SAILOR_API void         ImGui_RenderDrawData(ImDrawData* draw_data, RHI::RHICommandListPtr transferCmdList, RHI::RHICommandListPtr drawCmdList, VkPipeline pipeline = VK_NULL_HANDLE);
	SAILOR_API bool         ImGui_CreateFontsTexture(RHI::RHICommandListPtr command_buffer);
	SAILOR_API void         ImGui_DestroyFontUploadObjects();
	SAILOR_API void         ImGui_SetMinImageCount(uint32_t min_image_count); // To override MinImageCount after initialization (e.g. if swap chain is recreated)

	SAILOR_API VkDescriptorSet ImGui_AddTexture(VkSampler sampler, VkImageView image_view, VkImageLayout image_layout);
	SAILOR_API void            ImGui_RemoveTexture(VkDescriptorSet descriptor_set);
}