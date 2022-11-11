#include "Core/LogMacros.h"
#include "Tasks/Scheduler.h"
#include "ImGuiApi.h"

#ifdef  SAILOR_BUILD_WITH_VULKAN
#include "GraphicsDriver/Vulkan/VulkanImGui.h"
#include "GraphicsDriver/Vulkan/VulkanQueue.h"
#include "GraphicsDriver/Vulkan/VulkanRenderPass.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"

#include "RHI/CommandList.h"
#include "backends/imgui_impl_win32.h"
#endif

using namespace Sailor;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void ImGuiApi::HandleWin32(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
}

ImGuiApi::ImGuiApi(void* hWnd)
{
#ifdef  SAILOR_BUILD_WITH_VULKAN

	// the size of the pool is very oversize, but it's copied from imgui demo itself.
	VkDescriptorPoolSize poolSizes[] =
	{
		{ VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
	};

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	poolInfo.maxSets = 1000;
	poolInfo.poolSizeCount = (uint32_t)std::size(poolSizes);
	poolInfo.pPoolSizes = poolSizes;

	auto instance = GraphicsDriver::Vulkan::VulkanApi::GetInstance();
	auto device = instance->GetMainDevice();

	VkDescriptorPool imguiPool;
	VK_CHECK(vkCreateDescriptorPool(*device, &poolInfo, nullptr, &imguiPool));

	ImGui::CreateContext();

	ImGui_ImplWin32_Init(hWnd);

	Sailor::GraphicsDriver::Vulkan::ImGui_InitInfo initVulkanInfo = {};
	initVulkanInfo.Instance = instance->GetVkInstance();
	initVulkanInfo.Device = device;
	initVulkanInfo.DescriptorPool = imguiPool;
	initVulkanInfo.MinImageCount = 3;
	initVulkanInfo.ImageCount = 3;

	// We are only renders into frame buffer
	initVulkanInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	Sailor::GraphicsDriver::Vulkan::ImGui_Init(&initVulkanInfo, *device->GetRenderPass());

	auto& renderer = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	RHI::RHICommandListPtr cmdList = renderer->CreateCommandList();
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);

	Sailor::GraphicsDriver::Vulkan::ImGui_CreateFontsTexture(cmdList);

	RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);
	renderer->SubmitCommandList_Immediate(cmdList);

	m_releaseRhiResources = [=]()
	{
		auto instance = GraphicsDriver::Vulkan::VulkanApi::GetInstance();
		auto device = instance->GetMainDevice();

		vkDestroyDescriptorPool(*device, imguiPool, nullptr);
	};

#endif //  SAILOR_BUILD_WITH_VULKAN
}

ImGuiApi::~ImGuiApi()
{
#ifdef  SAILOR_BUILD_WITH_VULKAN

	ImGui_ImplWin32_Shutdown();

	SAILOR_ENQUEUE_TASK_RENDER_THREAD("Release ImGui",
		([releaseRhiResources = std::move(m_releaseRhiResources)]()
			{
				releaseRhiResources();
				Sailor::GraphicsDriver::Vulkan::ImGui_Shutdown();
			}
	));

#endif //  SAILOR_BUILD_WITH_VULKAN
}

void ImGuiApi::NewFrame()
{
	Sailor::GraphicsDriver::Vulkan::ImGui_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
}

void ImGuiApi::RenderFrame(RHI::RHICommandListPtr transferCmdList, RHI::RHICommandListPtr drawCmdList)
{
	SAILOR_PROFILE_FUNCTION();

	ImGui::Render();

#ifdef  SAILOR_BUILD_WITH_VULKAN
	Sailor::GraphicsDriver::Vulkan::ImGui_RenderDrawData(ImGui::GetDrawData(), transferCmdList, drawCmdList);
#endif //  SAILOR_BUILD_WITH_VULKAN
}