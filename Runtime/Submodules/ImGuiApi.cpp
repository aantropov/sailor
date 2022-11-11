#include "Core/LogMacros.h"
#include "Tasks/Scheduler.h"
#include "ImGuiApi.h"

#ifdef  SAILOR_BUILD_WITH_VULKAN
#include "GraphicsDriver/Vulkan/VulkanImGui.h"
#include "GraphicsDriver/Vulkan/VulkanQueue.h"
#include "GraphicsDriver/Vulkan/VulkanRenderPass.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "GraphicsDriver/Vulkan/VulkanImGui.h"

#include "RHI/CommandList.h"
#include "backends/imgui_impl_win32.h"
#endif

using namespace Sailor;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void ImGuiApi::HandleWin32(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	SAILOR_PROFILE_FUNCTION();

	ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
}

ImGuiApi::ImGuiApi(void* hWnd)
{
	SAILOR_PROFILE_FUNCTION();

#ifdef  SAILOR_BUILD_WITH_VULKAN

	auto instance = GraphicsDriver::Vulkan::VulkanApi::GetInstance();
	auto device = instance->GetMainDevice();

	ImGui::CreateContext();
	ImGui_ImplWin32_Init(hWnd);

	Sailor::GraphicsDriver::Vulkan::ImGui_InitInfo initVulkanInfo = {};
	initVulkanInfo.Instance = instance->GetVkInstance();
	initVulkanInfo.Device = device;
	initVulkanInfo.MinImageCount = 3;
	initVulkanInfo.ImageCount = 3;

	Sailor::GraphicsDriver::Vulkan::ImGui_Init(&initVulkanInfo);
	/*
	auto& renderer = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	RHI::RHICommandListPtr cmdList = renderer->CreateCommandList();
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);

	Sailor::GraphicsDriver::Vulkan::ImGui_CreateFontsTexture(cmdList);

	RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);
	renderer->SubmitCommandList_Immediate(cmdList);
	*/
#endif //  SAILOR_BUILD_WITH_VULKAN
}

ImGuiApi::~ImGuiApi()
{
	SAILOR_PROFILE_FUNCTION();

#ifdef  SAILOR_BUILD_WITH_VULKAN

	ImGui_ImplWin32_Shutdown();
	SAILOR_ENQUEUE_TASK_RENDER_THREAD("Release ImGui", ([]() { Sailor::GraphicsDriver::Vulkan::ImGui_Shutdown(); }));

#endif //  SAILOR_BUILD_WITH_VULKAN
}

void ImGuiApi::NewFrame()
{
	SAILOR_PROFILE_FUNCTION();

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