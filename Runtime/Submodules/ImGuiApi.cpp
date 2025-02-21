#include "ImGuiApi.h"

#include <stdio.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include "Core/LogMacros.h"
#include "Tasks/Scheduler.h"
#include "AssetRegistry/AssetRegistry.h"

#include "Memory/MemoryBlockAllocator.hpp"
#include "RHI/Renderer.h"
#include "RHI/GraphicsDriver.h"
#include "RHI/CommandList.h"
#include "RHI/VertexDescription.h"

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

	ImGui::CreateContext();
	ImGui_ImplWin32_Init(hWnd);

	InitInfo initInfo = {};
	initInfo.MinImageCount = 3;
	initInfo.ImageCount = 3;

	ImGui_Init(&initInfo);
}

ImGuiApi::~ImGuiApi()
{
	SAILOR_PROFILE_FUNCTION();

	ImGui_ImplWin32_Shutdown();
	ImGuiApi::ImGui_Shutdown();
}

void ImGuiApi::NewFrame()
{
	SAILOR_PROFILE_FUNCTION();

	Data* bd = ImGui_GetBackendData();
	IM_ASSERT(bd != nullptr && "Did you call ImGui_Init()?");
	IM_UNUSED(bd);

	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
}

void ImGuiApi::PrepareFrame(RHI::RHICommandListPtr transferCmdList)
{
	SAILOR_PROFILE_FUNCTION();
	ImGui::Render();
	ImGui_UpdateDrawData(ImGui::GetDrawData(), transferCmdList);
}

void ImGuiApi::RenderFrame(RHI::RHICommandListPtr drawCmdList)
{
	SAILOR_PROFILE_FUNCTION();

	ImGui_RenderDrawData(ImGui::GetDrawData(), drawCmdList);
}

ImGuiApi::Data* ImGuiApi::ImGui_GetBackendData()
{
	return ImGui::GetCurrentContext() ? (Data*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

void ImGuiApi::CreateOrResizeBuffer(RHI::RHIBufferPtr& buffer, size_t newSize)
{
	SAILOR_PROFILE_FUNCTION();

	Data* bd = ImGui_GetBackendData();
	const size_t vertex_buffer_size_aligned = ((newSize - 1) / bd->BufferMemoryAlignment + 1) * bd->BufferMemoryAlignment;

	auto& renderer = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	buffer = renderer->CreateBuffer(vertex_buffer_size_aligned,
		RHI::EBufferUsageBit::BufferTransferDst_Bit | RHI::EBufferUsageBit::VertexBuffer_Bit | RHI::EBufferUsageBit::IndexBuffer_Bit);
}

void ImGuiApi::ImGui_SetupRenderState(ImDrawData* drawData, RHI::RHICommandListPtr cmdList, const FrameRenderBuffers* rb, int width, int height)
{
	SAILOR_PROFILE_FUNCTION();

	Data* bd = ImGui_GetBackendData();

	RHI::Renderer::GetDriverCommands()->BindMaterial(cmdList, bd->Material);

	// Bind Vertex And Index Buffer:
	if (drawData->TotalVtxCount > 0)
	{
		const bool bUint16InsteadOfUint32 = sizeof(ImDrawIdx) == 2;
		RHI::Renderer::GetDriverCommands()->BindVertexBuffer(cmdList, rb->VertexBuffer, 0);
		RHI::Renderer::GetDriverCommands()->BindIndexBuffer(cmdList, rb->IndexBuffer, 0, bUint16InsteadOfUint32);
	}

	RHI::Renderer::GetDriverCommands()->SetViewport(cmdList,
		0, 0,
		(float)width, (float)height,
		glm::vec2(0, 0),
		glm::vec2(width, height),
		0, 1.0f);

	// Setup scale and translation:
	// Our visible imgui space lies from drawData->DisplayPps (top left) to drawData->DisplayPos+data_data->DisplaySize (bottom right). DisplayPos is (0,0) for single viewport apps.
	{
		float pushConstants[4];
		pushConstants[0] = 2.0f / drawData->DisplaySize.x; // scale x
		pushConstants[1] = 2.0f / drawData->DisplaySize.y; // scale y

		pushConstants[2] = -1.0f - drawData->DisplayPos.x * pushConstants[0]; // translate x
		pushConstants[3] = -1.0f - drawData->DisplayPos.y * pushConstants[1]; // translate y

		RHI::Renderer::GetDriverCommands()->PushConstants(cmdList, bd->Material, sizeof(float) * 4, pushConstants);
	}
}

void ImGuiApi::ImGui_UpdateDrawData(ImDrawData* drawData, RHI::RHICommandListPtr transferCmdList)
{
	SAILOR_PROFILE_FUNCTION();

	// Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
	const int width = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
	const int height = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);

	if (width <= 0 || height <= 0)
		return;

	Data* bd = ImGui_GetBackendData();
	InitInfo* v = &bd->InitInfo;

	// Allocate array to store enough vertex/index buffers
	WindowRenderBuffers* wrb = &bd->MainWindowRenderBuffers;
	if (wrb->FrameRenderBuffers == nullptr)
	{
		wrb->Index = 0;
		wrb->Count = v->ImageCount;
		wrb->FrameRenderBuffers = (FrameRenderBuffers*)IM_ALLOC(sizeof(FrameRenderBuffers) * wrb->Count);
		memset(wrb->FrameRenderBuffers, 0, sizeof(FrameRenderBuffers) * wrb->Count);
	}
	IM_ASSERT(wrb->Count == v->ImageCount);
	wrb->Index = (wrb->Index + 1) % wrb->Count;
	FrameRenderBuffers* rb = &wrb->FrameRenderBuffers[wrb->Index];

	if (drawData->TotalVtxCount > 0)
	{
		// Create or resize the vertex/index buffers
		size_t vertex_size = drawData->TotalVtxCount * sizeof(ImDrawVert);
		size_t index_size = drawData->TotalIdxCount * sizeof(ImDrawIdx);
		if (rb->VertexBuffer == nullptr || rb->VertexBuffer->GetSize() < vertex_size)
		{
			CreateOrResizeBuffer(rb->VertexBuffer, vertex_size);
		}
		if (rb->IndexBuffer == nullptr || rb->IndexBuffer->GetSize() < index_size)
		{
			CreateOrResizeBuffer(rb->IndexBuffer, index_size);
		}

		size_t vOffset = 0;
		size_t iOffset = 0;

		for (int n = 0; n < drawData->CmdListsCount; n++)
		{
			const ImDrawList* cmd_list = drawData->CmdLists[n];

			const auto vSize = cmd_list->VtxBuffer.Size * sizeof(ImDrawVert);
			const auto iSize = cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx);

			RHI::Renderer::GetDriverCommands()->UpdateBuffer(transferCmdList, rb->VertexBuffer, cmd_list->VtxBuffer.Data, vSize, vOffset);
			RHI::Renderer::GetDriverCommands()->UpdateBuffer(transferCmdList, rb->IndexBuffer, cmd_list->IdxBuffer.Data, iSize, iOffset);

			vOffset += vSize;
			iOffset += iSize;
		}
	}

}

void ImGuiApi::ImGui_RenderDrawData(ImDrawData* drawData, RHI::RHICommandListPtr drawCmdList)
{
	SAILOR_PROFILE_FUNCTION();

	if (drawData == nullptr)
		return;

	// Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
	const int width = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
	const int height = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);

	if (width <= 0 || height <= 0)
		return;

	Data* bd = ImGui_GetBackendData();
	WindowRenderBuffers* wrb = &bd->MainWindowRenderBuffers;
	const FrameRenderBuffers* rb = &wrb->FrameRenderBuffers[wrb->Index];

	ImGui_SetupRenderState(drawData, drawCmdList, rb, width, height);

	// Will project scissor/clipping rectangles into framebuffer space
	ImVec2 clipOff = drawData->DisplayPos;         // (0,0) unless using multi-viewports
	ImVec2 clipScale = drawData->FramebufferScale; // (1,1) unless using retina display which are often (2,2)

	// Render command lists
	// (Because we merged all buffers into a single one, we maintain our own offset into them)
	int globalVtxOffset = 0;
	int globalIdxOffset = 0;
	for (int n = 0; n < drawData->CmdListsCount; n++)
	{
		const ImDrawList* cmd_list = drawData->CmdLists[n];
		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
		{
			const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
			if (pcmd->UserCallback != nullptr)
			{
				// User callback, registered via ImDrawList::AddCallback()
				// (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
				if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
					ImGui_SetupRenderState(drawData, drawCmdList, rb, width, height);
				else
					pcmd->UserCallback(cmd_list, pcmd);
			}
			else
			{
				// Project scissor/clipping rectangles into framebuffer space
				ImVec2 clipMin((pcmd->ClipRect.x - clipOff.x) * clipScale.x, (pcmd->ClipRect.y - clipOff.y) * clipScale.y);
				ImVec2 clipMax((pcmd->ClipRect.z - clipOff.x) * clipScale.x, (pcmd->ClipRect.w - clipOff.y) * clipScale.y);

				// Clamp to viewport as vkCmdSetScissor() won't accept values that are off bounds
				if (clipMin.x < 0.0f) { clipMin.x = 0.0f; }
				if (clipMin.y < 0.0f) { clipMin.y = 0.0f; }
				if (clipMax.x > width) { clipMax.x = (float)width; }
				if (clipMax.y > height) { clipMax.y = (float)height; }
				if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y)
					continue;

				RHI::Renderer::GetDriverCommands()->SetViewport(drawCmdList,
					0, 0,
					(float)width, (float)height,
					glm::ivec2((int32_t)clipMin.x, (int32_t)clipMin.y),
					glm::ivec2((int32_t)(clipMax.x - clipMin.x), (int32_t)(clipMax.y - clipMin.y)),
					0, 1.0f);

				// Bind DescriptorSet with font or user texture
				// TODO: Support custom textures
				/*VkDescriptorSet desc_set[1] = {(VkDescriptorSet)pcmd->TextureId};
				if (sizeof(ImTextureID) < sizeof(ImU64))
				{
					// We don't support texture switches if ImTextureID hasn't been redefined to be 64-bit. Do a flaky check that other textures haven't been used.
					//IM_ASSERT(pcmd->TextureId == (ImTextureID)bd->FontDescriptorSet);
					desc_set[0] = *bd->Material->GetBindings()->m_vulkan.m_descriptorSet;
				}*/

				RHI::Renderer::GetDriverCommands()->BindShaderBindings(drawCmdList, bd->Material, { bd->ShaderBindings });
				RHI::Renderer::GetDriverCommands()->DrawIndexed(drawCmdList,
					pcmd->ElemCount,
					1,
					pcmd->IdxOffset + globalIdxOffset,
					pcmd->VtxOffset + globalVtxOffset,
					0);
			}
		}
		globalIdxOffset += cmd_list->IdxBuffer.Size;
		globalVtxOffset += cmd_list->VtxBuffer.Size;
	}
}

bool ImGuiApi::ImGui_Init(InitInfo* info)
{
	ImGuiIO& io = ImGui::GetIO();
	IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");

	// Setup backend capabilities flags
	Data* bd = IM_NEW(Data)();
	io.BackendRendererUserData = (void*)bd;
	io.BackendRendererName = "imgui_impl_sailor";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.

	IM_ASSERT(info->MinImageCount >= 2);
	IM_ASSERT(info->ImageCount >= info->MinImageCount);

	bd->InitInfo = *info;
	bd->Subpass = info->Subpass;

	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	size_t upload_size = width * height * 4 * sizeof(char);

	bd->FontTexture = RHI::Renderer::GetDriver()->CreateTexture(pixels,
		upload_size,
		glm::ivec3(width, height, 1),
		1,
		RHI::ETextureType::Texture2D,
		RHI::ETextureFormat::R8G8B8A8_UNORM,
		RHI::ETextureFiltration::Linear,
		RHI::ETextureClamping::Repeat,
		RHI::ETextureUsageBit::Sampled_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit);

	io.Fonts->SetTexID((ImTextureID)(bd->FontTexture.GetRawPtr()));

	bd->ShaderBindings = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
	RHI::RHIShaderBindingPtr textureSampler = Sailor::RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(bd->ShaderBindings, "sTexture", bd->FontTexture, 0);

	if (auto uiShaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ImGuiUI.shader"))
	{
		App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(uiShaderInfo->GetFileId(), bd->Shader);
	}

	check(bd->Shader.IsValid());

	RHI::RenderState renderState = RHI::RenderState(false, false, 0.0f, false, RHI::ECullMode::None,
		RHI::EBlendMode::AlphaBlending, RHI::EFillMode::Fill, 0, false);

	RHI::RHIVertexDescriptionPtr vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP2UV2C1>();

	bd->Material = RHI::Renderer::GetDriver()->CreateMaterial(
		vertexDescription,
		RHI::EPrimitiveTopology::TriangleList,
		renderState,
		bd->Shader,
		bd->ShaderBindings);

	return true;
}

void ImGuiApi::ImGui_Shutdown()
{
	Data* bd = ImGui_GetBackendData();
	IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
	ImGuiIO& io = ImGui::GetIO();

	DestroyWindowRenderBuffers(&bd->MainWindowRenderBuffers);

	bd->Shader.Clear();
	bd->Material.Clear();
	bd->ShaderBindings.Clear();
	bd->FontTexture.Clear();

	io.BackendRendererName = nullptr;
	io.BackendRendererUserData = nullptr;
	IM_DELETE(bd);
}

void ImGuiApi::ImGui_SetMinImageCount(uint32_t min_image_count)
{
	Data* bd = ImGui_GetBackendData();
	IM_ASSERT(min_image_count >= 2);
	if (bd->InitInfo.MinImageCount == min_image_count)
		return;

	RHI::Renderer::GetDriver()->WaitIdle();
	DestroyWindowRenderBuffers(&bd->MainWindowRenderBuffers);
	bd->InitInfo.MinImageCount = min_image_count;
}

void ImGuiApi::DestroyWindowRenderBuffers(WindowRenderBuffers* buffers)
{
	for (uint32_t n = 0; n < buffers->Count; n++)
	{
		buffers->FrameRenderBuffers[n].VertexBuffer.Clear();
		buffers->FrameRenderBuffers[n].IndexBuffer.Clear();
	}

	IM_FREE(buffers->FrameRenderBuffers);
	buffers->FrameRenderBuffers = nullptr;
	buffers->Index = 0;
	buffers->Count = 0;
}