#pragma once
#include "imgui.h"
#include "VulkanApi.h"
#include "VulkanDevice.h"

namespace Sailor::GraphicsDriver::Vulkan
{
	struct ImGui_InitInfo
	{
		VkInstance                      Instance{};
		VulkanDevicePtr                 Device{};
		uint32_t                        Subpass{};
		uint32_t                        MinImageCount{}; // >= 2
		uint32_t                        ImageCount{};    // >= MinImageCount
		const VkAllocationCallbacks* Allocator{};
	};

	struct ImGuiH_FrameRenderBuffers
	{
		RHI::RHIBufferPtr   VertexBuffer;
		RHI::RHIBufferPtr   IndexBuffer;
	};

	struct ImGuiH_WindowRenderBuffers
	{
		uint32_t            Index;
		uint32_t            Count;
		ImGuiH_FrameRenderBuffers* FrameRenderBuffers;
	};

	struct ImGui_Data
	{
		ImGui_InitInfo				VulkanInitInfo;
		VkDeviceSize                BufferMemoryAlignment;
		RHI::RHIMaterialPtr         Material;
		uint32_t                    Subpass;
		ShaderSetPtr                Shader;
		RHI::RHIShaderBindingSetPtr ShaderBindings;
		RHI::RHITexturePtr			FontTexture;
		ImGuiH_WindowRenderBuffers MainWindowRenderBuffers;

		ImGui_Data() { memset((void*)this, 0, sizeof(*this)); BufferMemoryAlignment = 256; }
	};

	// Forward Declarations
	void ImGuiH_DestroyWindowRenderBuffers(VulkanDevicePtr device, ImGuiH_WindowRenderBuffers* buffers, const VkAllocationCallbacks* allocator);

	SAILOR_API bool         ImGui_Init(ImGui_InitInfo* info);
	SAILOR_API void         ImGui_NewFrame();
	SAILOR_API void         ImGui_Shutdown();
	SAILOR_API void         ImGui_RenderDrawData(ImDrawData* draw_data, RHI::RHICommandListPtr transferCmdList, RHI::RHICommandListPtr drawCmdList, VulkanGraphicsPipelinePtr pipeline = VK_NULL_HANDLE);
	SAILOR_API void         ImGui_SetMinImageCount(uint32_t min_image_count); // To override MinImageCount after initialization (e.g. if swap chain is recreated)
}