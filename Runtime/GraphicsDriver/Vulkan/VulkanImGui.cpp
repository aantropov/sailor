#include "VulkanImGui.h"
#include "VulkanApi.h"
#include "VulkanDevice.h"
#include "VulkanBuffer.h"
#include "VulkanCommandBuffer.h"
#include "Memory/MemoryBLockAllocator.hpp"
#include "RHI/Renderer.h"
#include "RHI/GraphicsDriver.h"
#include "RHI/CommandList.h"
#include <stdio.h>

// Visual Studio warnings
#ifdef _MSC_VER
#pragma warning (disable: 4127) // condition expression is constant
#endif

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace Sailor::GraphicsDriver::Vulkan
{
	// Reusable buffers used for rendering 1 current in-flight frame, for ImGui_RenderDrawData()
	// [Please zero-clear before use!]
	struct ImGuiH_FrameRenderBuffers
	{
		VkDeviceSize        VertexBufferSize;
		VkDeviceSize        IndexBufferSize;
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
		ImGui_InitInfo   VulkanInitInfo;
		VkRenderPass                RenderPass;
		VkDeviceSize                BufferMemoryAlignment;
		VkPipelineCreateFlags       PipelineCreateFlags;
		VkDescriptorSetLayout       DescriptorSetLayout;
		VkPipelineLayout            PipelineLayout;
		VkPipeline                  Pipeline;
		uint32_t                    Subpass;
		VkShaderModule              ShaderModuleVert;
		VkShaderModule              ShaderModuleFrag;

		// Font data
		VkSampler                   FontSampler;
		VkDeviceMemory              FontMemory;
		VkImage                     FontImage;
		VkImageView                 FontView;
		VkDescriptorSet             FontDescriptorSet;
		VkDeviceMemory              UploadBufferMemory;
		VkBuffer                    UploadBuffer;

		// Render buffers
		ImGuiH_WindowRenderBuffers MainWindowRenderBuffers;

		ImGui_Data()
		{
			memset((void*)this, 0, sizeof(*this));
			BufferMemoryAlignment = 256;
		}
	};

	// Forward Declarations
	bool ImGui_CreateDeviceObjects();
	void ImGui_DestroyDeviceObjects();
	void ImGuiH_DestroyFrameRenderBuffers(VulkanDevicePtr device, ImGuiH_FrameRenderBuffers* buffers, const VkAllocationCallbacks* allocator);
	void ImGuiH_DestroyWindowRenderBuffers(VulkanDevicePtr device, ImGuiH_WindowRenderBuffers* buffers, const VkAllocationCallbacks* allocator);

	//-----------------------------------------------------------------------------
	// SHADERS
	//-----------------------------------------------------------------------------

	// glsl_shader.vert, compiled with:
	// # glslangValidator -V -x -o glsl_shader.vert.u32 glsl_shader.vert
	/*
	#version 450 core
	layout(location = 0) in vec2 aPos;
	layout(location = 1) in vec2 aUV;
	layout(location = 2) in vec4 aColor;
	layout(push_constant) uniform uPushConstant { vec2 uScale; vec2 uTranslate; } pc;

	out gl_PerVertex { vec4 gl_Position; };
	layout(location = 0) out struct { vec4 Color; vec2 UV; } Out;

	void main()
	{
		Out.Color = aColor;
		Out.UV = aUV;
		gl_Position = vec4(aPos * pc.uScale + pc.uTranslate, 0, 1);
	}
	*/
	static uint32_t __glsl_shader_vert_spv[] =
	{
		0x07230203,0x00010000,0x00080001,0x0000002e,0x00000000,0x00020011,0x00000001,0x0006000b,
		0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
		0x000a000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000b,0x0000000f,0x00000015,
		0x0000001b,0x0000001c,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,
		0x00000000,0x00030005,0x00000009,0x00000000,0x00050006,0x00000009,0x00000000,0x6f6c6f43,
		0x00000072,0x00040006,0x00000009,0x00000001,0x00005655,0x00030005,0x0000000b,0x0074754f,
		0x00040005,0x0000000f,0x6c6f4361,0x0000726f,0x00030005,0x00000015,0x00565561,0x00060005,
		0x00000019,0x505f6c67,0x65567265,0x78657472,0x00000000,0x00060006,0x00000019,0x00000000,
		0x505f6c67,0x7469736f,0x006e6f69,0x00030005,0x0000001b,0x00000000,0x00040005,0x0000001c,
		0x736f5061,0x00000000,0x00060005,0x0000001e,0x73755075,0x6e6f4368,0x6e617473,0x00000074,
		0x00050006,0x0000001e,0x00000000,0x61635375,0x0000656c,0x00060006,0x0000001e,0x00000001,
		0x61725475,0x616c736e,0x00006574,0x00030005,0x00000020,0x00006370,0x00040047,0x0000000b,
		0x0000001e,0x00000000,0x00040047,0x0000000f,0x0000001e,0x00000002,0x00040047,0x00000015,
		0x0000001e,0x00000001,0x00050048,0x00000019,0x00000000,0x0000000b,0x00000000,0x00030047,
		0x00000019,0x00000002,0x00040047,0x0000001c,0x0000001e,0x00000000,0x00050048,0x0000001e,
		0x00000000,0x00000023,0x00000000,0x00050048,0x0000001e,0x00000001,0x00000023,0x00000008,
		0x00030047,0x0000001e,0x00000002,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,
		0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040017,
		0x00000008,0x00000006,0x00000002,0x0004001e,0x00000009,0x00000007,0x00000008,0x00040020,
		0x0000000a,0x00000003,0x00000009,0x0004003b,0x0000000a,0x0000000b,0x00000003,0x00040015,
		0x0000000c,0x00000020,0x00000001,0x0004002b,0x0000000c,0x0000000d,0x00000000,0x00040020,
		0x0000000e,0x00000001,0x00000007,0x0004003b,0x0000000e,0x0000000f,0x00000001,0x00040020,
		0x00000011,0x00000003,0x00000007,0x0004002b,0x0000000c,0x00000013,0x00000001,0x00040020,
		0x00000014,0x00000001,0x00000008,0x0004003b,0x00000014,0x00000015,0x00000001,0x00040020,
		0x00000017,0x00000003,0x00000008,0x0003001e,0x00000019,0x00000007,0x00040020,0x0000001a,
		0x00000003,0x00000019,0x0004003b,0x0000001a,0x0000001b,0x00000003,0x0004003b,0x00000014,
		0x0000001c,0x00000001,0x0004001e,0x0000001e,0x00000008,0x00000008,0x00040020,0x0000001f,
		0x00000009,0x0000001e,0x0004003b,0x0000001f,0x00000020,0x00000009,0x00040020,0x00000021,
		0x00000009,0x00000008,0x0004002b,0x00000006,0x00000028,0x00000000,0x0004002b,0x00000006,
		0x00000029,0x3f800000,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,
		0x00000005,0x0004003d,0x00000007,0x00000010,0x0000000f,0x00050041,0x00000011,0x00000012,
		0x0000000b,0x0000000d,0x0003003e,0x00000012,0x00000010,0x0004003d,0x00000008,0x00000016,
		0x00000015,0x00050041,0x00000017,0x00000018,0x0000000b,0x00000013,0x0003003e,0x00000018,
		0x00000016,0x0004003d,0x00000008,0x0000001d,0x0000001c,0x00050041,0x00000021,0x00000022,
		0x00000020,0x0000000d,0x0004003d,0x00000008,0x00000023,0x00000022,0x00050085,0x00000008,
		0x00000024,0x0000001d,0x00000023,0x00050041,0x00000021,0x00000025,0x00000020,0x00000013,
		0x0004003d,0x00000008,0x00000026,0x00000025,0x00050081,0x00000008,0x00000027,0x00000024,
		0x00000026,0x00050051,0x00000006,0x0000002a,0x00000027,0x00000000,0x00050051,0x00000006,
		0x0000002b,0x00000027,0x00000001,0x00070050,0x00000007,0x0000002c,0x0000002a,0x0000002b,
		0x00000028,0x00000029,0x00050041,0x00000011,0x0000002d,0x0000001b,0x0000000d,0x0003003e,
		0x0000002d,0x0000002c,0x000100fd,0x00010038
	};

	// glsl_shader.frag, compiled with:
	// # glslangValidator -V -x -o glsl_shader.frag.u32 glsl_shader.frag
	/*
	#version 450 core
	layout(location = 0) out vec4 fColor;
	layout(set=0, binding=0) uniform sampler2D sTexture;
	layout(location = 0) in struct { vec4 Color; vec2 UV; } In;
	void main()
	{
		fColor = In.Color * texture(sTexture, In.UV.st);
	}
	*/
	static uint32_t __glsl_shader_frag_spv[] =
	{
		0x07230203,0x00010000,0x00080001,0x0000001e,0x00000000,0x00020011,0x00000001,0x0006000b,
		0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
		0x0007000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x0000000d,0x00030010,
		0x00000004,0x00000007,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,
		0x00000000,0x00040005,0x00000009,0x6c6f4366,0x0000726f,0x00030005,0x0000000b,0x00000000,
		0x00050006,0x0000000b,0x00000000,0x6f6c6f43,0x00000072,0x00040006,0x0000000b,0x00000001,
		0x00005655,0x00030005,0x0000000d,0x00006e49,0x00050005,0x00000016,0x78655473,0x65727574,
		0x00000000,0x00040047,0x00000009,0x0000001e,0x00000000,0x00040047,0x0000000d,0x0000001e,
		0x00000000,0x00040047,0x00000016,0x00000022,0x00000000,0x00040047,0x00000016,0x00000021,
		0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,
		0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,0x00000003,
		0x00000007,0x0004003b,0x00000008,0x00000009,0x00000003,0x00040017,0x0000000a,0x00000006,
		0x00000002,0x0004001e,0x0000000b,0x00000007,0x0000000a,0x00040020,0x0000000c,0x00000001,
		0x0000000b,0x0004003b,0x0000000c,0x0000000d,0x00000001,0x00040015,0x0000000e,0x00000020,
		0x00000001,0x0004002b,0x0000000e,0x0000000f,0x00000000,0x00040020,0x00000010,0x00000001,
		0x00000007,0x00090019,0x00000013,0x00000006,0x00000001,0x00000000,0x00000000,0x00000000,
		0x00000001,0x00000000,0x0003001b,0x00000014,0x00000013,0x00040020,0x00000015,0x00000000,
		0x00000014,0x0004003b,0x00000015,0x00000016,0x00000000,0x0004002b,0x0000000e,0x00000018,
		0x00000001,0x00040020,0x00000019,0x00000001,0x0000000a,0x00050036,0x00000002,0x00000004,
		0x00000000,0x00000003,0x000200f8,0x00000005,0x00050041,0x00000010,0x00000011,0x0000000d,
		0x0000000f,0x0004003d,0x00000007,0x00000012,0x00000011,0x0004003d,0x00000014,0x00000017,
		0x00000016,0x00050041,0x00000019,0x0000001a,0x0000000d,0x00000018,0x0004003d,0x0000000a,
		0x0000001b,0x0000001a,0x00050057,0x00000007,0x0000001c,0x00000017,0x0000001b,0x00050085,
		0x00000007,0x0000001d,0x00000012,0x0000001c,0x0003003e,0x00000009,0x0000001d,0x000100fd,
		0x00010038
	};

	static ImGui_Data* ImGui_GetBackendData()
	{
		return ImGui::GetCurrentContext() ? (ImGui_Data*)ImGui::GetIO().BackendRendererUserData : nullptr;
	}

	static uint32_t ImGui_MemoryType(VkMemoryPropertyFlags properties, uint32_t type_bits)
	{
		ImGui_Data* bd = ImGui_GetBackendData();
		ImGui_InitInfo* v = &bd->VulkanInitInfo;
		VkPhysicalDeviceMemoryProperties prop;
		vkGetPhysicalDeviceMemoryProperties(v->Device->GetPhysicalDevice(), &prop);
		for (uint32_t i = 0; i < prop.memoryTypeCount; i++)
			if ((prop.memoryTypes[i].propertyFlags & properties) == properties && type_bits & (1 << i))
				return i;
		return 0xFFFFFFFF; // Unable to find memoryType
	}

	static void check_vk_result(VkResult err)
	{
		ImGui_Data* bd = ImGui_GetBackendData();
		if (!bd)
			return;
		ImGui_InitInfo* v = &bd->VulkanInitInfo;
		if (v->CheckVkResultFn)
			v->CheckVkResultFn(err);
	}

	static void CreateOrResizeBuffer(RHI::RHIBufferPtr& buffer, size_t new_size, VkBufferUsageFlagBits usage)
	{
		SAILOR_PROFILE_FUNCTION();

		ImGui_Data* bd = ImGui_GetBackendData();
		ImGui_InitInfo* v = &bd->VulkanInitInfo;
		VkDeviceSize vertex_buffer_size_aligned = ((new_size - 1) / bd->BufferMemoryAlignment + 1) * bd->BufferMemoryAlignment;

		auto& renderer = App::GetSubmodule<RHI::Renderer>()->GetDriver();
		buffer = renderer->CreateBuffer(vertex_buffer_size_aligned, 
			RHI::EBufferUsageBit::BufferTransferDst_Bit | RHI::EBufferUsageBit::VertexBuffer_Bit | RHI::EBufferUsageBit::IndexBuffer_Bit);
	}

	static void ImGui_SetupRenderState(ImDrawData* draw_data, VkPipeline pipeline, RHI::RHICommandListPtr command_buffer, ImGuiH_FrameRenderBuffers* rb, int fb_width, int fb_height)
	{
		SAILOR_PROFILE_FUNCTION();

		ImGui_Data* bd = ImGui_GetBackendData();

		// Bind pipeline:
		{
			vkCmdBindPipeline(*command_buffer->m_vulkan.m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		}

		// Bind Vertex And Index Buffer:
		if (draw_data->TotalVtxCount > 0)
		{
			const bool bUint16InsteadOfUint32 = sizeof(ImDrawIdx) == 2;
			RHI::Renderer::GetDriverCommands()->BindVertexBuffer(command_buffer, rb->VertexBuffer, 0);
			RHI::Renderer::GetDriverCommands()->BindIndexBuffer(command_buffer, rb->IndexBuffer, 0, bUint16InsteadOfUint32);
		}

		// Setup viewport:
		{
			VkViewport viewport;
			viewport.x = 0;
			viewport.y = 0;
			viewport.width = (float)fb_width;
			viewport.height = (float)fb_height;
			viewport.minDepth = 0.0f;
			viewport.maxDepth = 1.0f;
			vkCmdSetViewport(*command_buffer->m_vulkan.m_commandBuffer, 0, 1, &viewport);
		}

		// Setup scale and translation:
		// Our visible imgui space lies from draw_data->DisplayPps (top left) to draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayPos is (0,0) for single viewport apps.
		{
			float scale[2];
			scale[0] = 2.0f / draw_data->DisplaySize.x;
			scale[1] = 2.0f / draw_data->DisplaySize.y;
			float translate[2];
			translate[0] = -1.0f - draw_data->DisplayPos.x * scale[0];
			translate[1] = -1.0f - draw_data->DisplayPos.y * scale[1];
			vkCmdPushConstants(*command_buffer->m_vulkan.m_commandBuffer, bd->PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, sizeof(float) * 0, sizeof(float) * 2, scale);
			vkCmdPushConstants(*command_buffer->m_vulkan.m_commandBuffer, bd->PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, sizeof(float) * 2, sizeof(float) * 2, translate);
		}
	}

	// Render function
	void ImGui_RenderDrawData(ImDrawData* draw_data, RHI::RHICommandListPtr transferCmdList, RHI::RHICommandListPtr drawCmdList, VkPipeline pipeline)
	{
		SAILOR_PROFILE_FUNCTION();

		// Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
		int fb_width = (int)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
		int fb_height = (int)(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
		if (fb_width <= 0 || fb_height <= 0)
			return;

		ImGui_Data* bd = ImGui_GetBackendData();
		ImGui_InitInfo* v = &bd->VulkanInitInfo;
		if (pipeline == VK_NULL_HANDLE)
			pipeline = bd->Pipeline;

		// Allocate array to store enough vertex/index buffers
		ImGuiH_WindowRenderBuffers* wrb = &bd->MainWindowRenderBuffers;
		if (wrb->FrameRenderBuffers == nullptr)
		{
			wrb->Index = 0;
			wrb->Count = v->ImageCount;
			wrb->FrameRenderBuffers = (ImGuiH_FrameRenderBuffers*)IM_ALLOC(sizeof(ImGuiH_FrameRenderBuffers) * wrb->Count);
			memset(wrb->FrameRenderBuffers, 0, sizeof(ImGuiH_FrameRenderBuffers) * wrb->Count);
		}
		IM_ASSERT(wrb->Count == v->ImageCount);
		wrb->Index = (wrb->Index + 1) % wrb->Count;
		ImGuiH_FrameRenderBuffers* rb = &wrb->FrameRenderBuffers[wrb->Index];

		if (draw_data->TotalVtxCount > 0)
		{
			// Create or resize the vertex/index buffers
			size_t vertex_size = draw_data->TotalVtxCount * sizeof(ImDrawVert);
			size_t index_size = draw_data->TotalIdxCount * sizeof(ImDrawIdx);
			if (rb->VertexBuffer == VK_NULL_HANDLE || rb->VertexBuffer->GetSize() < vertex_size)
			{
				CreateOrResizeBuffer(rb->VertexBuffer, vertex_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
				rb->VertexBufferSize = vertex_size;
			}
			if (rb->IndexBuffer == VK_NULL_HANDLE || rb->IndexBuffer->GetSize() < index_size)
			{
				CreateOrResizeBuffer(rb->IndexBuffer, index_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
				rb->IndexBufferSize = index_size;
			}

			size_t vOffset = 0;
			size_t iOffset = 0;

			for (int n = 0; n < draw_data->CmdListsCount; n++)
			{
				const ImDrawList* cmd_list = draw_data->CmdLists[n];

				const auto vSize = cmd_list->VtxBuffer.Size * sizeof(ImDrawVert);
				const auto iSize = cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx);

				RHI::Renderer::GetDriverCommands()->UpdateBuffer(transferCmdList, rb->VertexBuffer, cmd_list->VtxBuffer.Data, vSize, vOffset);
				RHI::Renderer::GetDriverCommands()->UpdateBuffer(transferCmdList, rb->IndexBuffer, cmd_list->IdxBuffer.Data, iSize, iOffset);

				vOffset += vSize;
				iOffset += iSize;
			}
		}

		// Setup desired Vulkan state
		ImGui_SetupRenderState(draw_data, pipeline, drawCmdList, rb, fb_width, fb_height);

		// Will project scissor/clipping rectangles into framebuffer space
		ImVec2 clip_off = draw_data->DisplayPos;         // (0,0) unless using multi-viewports
		ImVec2 clip_scale = draw_data->FramebufferScale; // (1,1) unless using retina display which are often (2,2)

		// Render command lists
		// (Because we merged all buffers into a single one, we maintain our own offset into them)
		int global_vtx_offset = 0;
		int global_idx_offset = 0;
		for (int n = 0; n < draw_data->CmdListsCount; n++)
		{
			const ImDrawList* cmd_list = draw_data->CmdLists[n];
			for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
			{
				const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
				if (pcmd->UserCallback != nullptr)
				{
					// User callback, registered via ImDrawList::AddCallback()
					// (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
					if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
						ImGui_SetupRenderState(draw_data, pipeline, drawCmdList, rb, fb_width, fb_height);
					else
						pcmd->UserCallback(cmd_list, pcmd);
				}
				else
				{
					// Project scissor/clipping rectangles into framebuffer space
					ImVec2 clip_min((pcmd->ClipRect.x - clip_off.x) * clip_scale.x, (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
					ImVec2 clip_max((pcmd->ClipRect.z - clip_off.x) * clip_scale.x, (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);

					// Clamp to viewport as vkCmdSetScissor() won't accept values that are off bounds
					if (clip_min.x < 0.0f) { clip_min.x = 0.0f; }
					if (clip_min.y < 0.0f) { clip_min.y = 0.0f; }
					if (clip_max.x > fb_width) { clip_max.x = (float)fb_width; }
					if (clip_max.y > fb_height) { clip_max.y = (float)fb_height; }
					if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
						continue;

					// Apply scissor/clipping rectangle
					VkRect2D scissor;
					scissor.offset.x = (int32_t)(clip_min.x);
					scissor.offset.y = (int32_t)(clip_min.y);
					scissor.extent.width = (uint32_t)(clip_max.x - clip_min.x);
					scissor.extent.height = (uint32_t)(clip_max.y - clip_min.y);
					vkCmdSetScissor(*drawCmdList->m_vulkan.m_commandBuffer, 0, 1, &scissor);

					// Bind DescriptorSet with font or user texture
					VkDescriptorSet desc_set[1] = { (VkDescriptorSet)pcmd->TextureId };
					if (sizeof(ImTextureID) < sizeof(ImU64))
					{
						// We don't support texture switches if ImTextureID hasn't been redefined to be 64-bit. Do a flaky check that other textures haven't been used.
						IM_ASSERT(pcmd->TextureId == (ImTextureID)bd->FontDescriptorSet);
						desc_set[0] = bd->FontDescriptorSet;
					}
					vkCmdBindDescriptorSets(*drawCmdList->m_vulkan.m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, bd->PipelineLayout, 0, 1, desc_set, 0, nullptr);

					RHI::Renderer::GetDriverCommands()->DrawIndexed(drawCmdList, 
						pcmd->ElemCount, 
						1, 
						pcmd->IdxOffset + global_idx_offset, 
						pcmd->VtxOffset + global_vtx_offset, 
						0);
				}
			}
			global_idx_offset += cmd_list->IdxBuffer.Size;
			global_vtx_offset += cmd_list->VtxBuffer.Size;
		}
	}

	bool ImGui_CreateFontsTexture(RHI::RHICommandListPtr command_buffer)
	{
		ImGuiIO& io = ImGui::GetIO();
		ImGui_Data* bd = ImGui_GetBackendData();
		ImGui_InitInfo* v = &bd->VulkanInitInfo;

		unsigned char* pixels;
		int width, height;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		size_t upload_size = width * height * 4 * sizeof(char);

		VkResult err;

		// Create the Image:
		{
			VkImageCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			info.imageType = VK_IMAGE_TYPE_2D;
			info.format = VK_FORMAT_R8G8B8A8_UNORM;
			info.extent.width = width;
			info.extent.height = height;
			info.extent.depth = 1;
			info.mipLevels = 1;
			info.arrayLayers = 1;
			info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.tiling = VK_IMAGE_TILING_OPTIMAL;
			info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			err = vkCreateImage(*v->Device, &info, v->Allocator, &bd->FontImage);
			check_vk_result(err);
			VkMemoryRequirements req;
			vkGetImageMemoryRequirements(*v->Device, bd->FontImage, &req);
			VkMemoryAllocateInfo alloc_info = {};
			alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			alloc_info.allocationSize = req.size;
			alloc_info.memoryTypeIndex = ImGui_MemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, req.memoryTypeBits);
			err = vkAllocateMemory(*v->Device, &alloc_info, v->Allocator, &bd->FontMemory);
			check_vk_result(err);
			err = vkBindImageMemory(*v->Device, bd->FontImage, bd->FontMemory, 0);
			check_vk_result(err);
		}

		// Create the Image View:
		{
			VkImageViewCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			info.image = bd->FontImage;
			info.viewType = VK_IMAGE_VIEW_TYPE_2D;
			info.format = VK_FORMAT_R8G8B8A8_UNORM;
			info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			info.subresourceRange.levelCount = 1;
			info.subresourceRange.layerCount = 1;
			err = vkCreateImageView(*v->Device, &info, v->Allocator, &bd->FontView);
			check_vk_result(err);
		}

		// Create the Descriptor Set:
		bd->FontDescriptorSet = (VkDescriptorSet)ImGui_AddTexture(bd->FontSampler, bd->FontView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// Create the Upload Buffer:
		{
			VkBufferCreateInfo buffer_info = {};
			buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			buffer_info.size = upload_size;
			buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			err = vkCreateBuffer(*v->Device, &buffer_info, v->Allocator, &bd->UploadBuffer);
			check_vk_result(err);
			VkMemoryRequirements req;
			vkGetBufferMemoryRequirements(*v->Device, bd->UploadBuffer, &req);
			bd->BufferMemoryAlignment = (bd->BufferMemoryAlignment > req.alignment) ? bd->BufferMemoryAlignment : req.alignment;
			VkMemoryAllocateInfo alloc_info = {};
			alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			alloc_info.allocationSize = req.size;
			alloc_info.memoryTypeIndex = ImGui_MemoryType(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, req.memoryTypeBits);
			err = vkAllocateMemory(*v->Device, &alloc_info, v->Allocator, &bd->UploadBufferMemory);
			check_vk_result(err);
			err = vkBindBufferMemory(*v->Device, bd->UploadBuffer, bd->UploadBufferMemory, 0);
			check_vk_result(err);
		}

		// Upload to Buffer:
		{
			char* map = nullptr;
			err = vkMapMemory(*v->Device, bd->UploadBufferMemory, 0, upload_size, 0, (void**)(&map));
			check_vk_result(err);
			memcpy(map, pixels, upload_size);
			VkMappedMemoryRange range[1] = {};
			range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			range[0].memory = bd->UploadBufferMemory;
			range[0].size = upload_size;
			err = vkFlushMappedMemoryRanges(*v->Device, 1, range);
			check_vk_result(err);
			vkUnmapMemory(*v->Device, bd->UploadBufferMemory);
		}

		// Copy to Image:
		{
			VkImageMemoryBarrier copy_barrier[1] = {};
			copy_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			copy_barrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			copy_barrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			copy_barrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			copy_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			copy_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			copy_barrier[0].image = bd->FontImage;
			copy_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copy_barrier[0].subresourceRange.levelCount = 1;
			copy_barrier[0].subresourceRange.layerCount = 1;
			vkCmdPipelineBarrier(*command_buffer->m_vulkan.m_commandBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, copy_barrier);

			VkBufferImageCopy region = {};
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.layerCount = 1;
			region.imageExtent.width = width;
			region.imageExtent.height = height;
			region.imageExtent.depth = 1;
			vkCmdCopyBufferToImage(*command_buffer->m_vulkan.m_commandBuffer, bd->UploadBuffer, bd->FontImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			VkImageMemoryBarrier use_barrier[1] = {};
			use_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			use_barrier[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			use_barrier[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			use_barrier[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			use_barrier[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			use_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			use_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			use_barrier[0].image = bd->FontImage;
			use_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			use_barrier[0].subresourceRange.levelCount = 1;
			use_barrier[0].subresourceRange.layerCount = 1;
			vkCmdPipelineBarrier(*command_buffer->m_vulkan.m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, use_barrier);
		}

		// Store our identifier
		io.Fonts->SetTexID((ImTextureID)bd->FontDescriptorSet);

		return true;
	}

	static void ImGui_CreateShaderModules(VulkanDevicePtr device, const VkAllocationCallbacks* allocator)
	{
		// Create the shader modules
		ImGui_Data* bd = ImGui_GetBackendData();
		if (bd->ShaderModuleVert == VK_NULL_HANDLE)
		{
			VkShaderModuleCreateInfo vert_info = {};
			vert_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			vert_info.codeSize = sizeof(__glsl_shader_vert_spv);
			vert_info.pCode = (uint32_t*)__glsl_shader_vert_spv;
			VkResult err = vkCreateShaderModule(*device, &vert_info, allocator, &bd->ShaderModuleVert);
			check_vk_result(err);
		}
		if (bd->ShaderModuleFrag == VK_NULL_HANDLE)
		{
			VkShaderModuleCreateInfo frag_info = {};
			frag_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			frag_info.codeSize = sizeof(__glsl_shader_frag_spv);
			frag_info.pCode = (uint32_t*)__glsl_shader_frag_spv;
			VkResult err = vkCreateShaderModule(*device, &frag_info, allocator, &bd->ShaderModuleFrag);
			check_vk_result(err);
		}
	}

	static void ImGui_CreateFontSampler(VulkanDevicePtr device, const VkAllocationCallbacks* allocator)
	{
		ImGui_Data* bd = ImGui_GetBackendData();
		if (bd->FontSampler)
			return;

		// Bilinear sampling is required by default. Set 'io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines' or 'style.AntiAliasedLinesUseTex = false' to allow point/nearest sampling.
		VkSamplerCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		info.magFilter = VK_FILTER_LINEAR;
		info.minFilter = VK_FILTER_LINEAR;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		info.minLod = -1000;
		info.maxLod = 1000;
		info.maxAnisotropy = 1.0f;
		VkResult err = vkCreateSampler(*device, &info, allocator, &bd->FontSampler);
		check_vk_result(err);
	}

	static void ImGui_CreateDescriptorSetLayout(VulkanDevicePtr device, const VkAllocationCallbacks* allocator)
	{
		ImGui_Data* bd = ImGui_GetBackendData();
		if (bd->DescriptorSetLayout)
			return;

		ImGui_CreateFontSampler(device, allocator);
		VkSampler sampler[1] = { bd->FontSampler };
		VkDescriptorSetLayoutBinding binding[1] = {};
		binding[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding[0].descriptorCount = 1;
		binding[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		binding[0].pImmutableSamplers = sampler;
		VkDescriptorSetLayoutCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		info.bindingCount = 1;
		info.pBindings = binding;
		VkResult err = vkCreateDescriptorSetLayout(*device, &info, allocator, &bd->DescriptorSetLayout);
		check_vk_result(err);
	}

	static void ImGui_CreatePipelineLayout(VulkanDevicePtr device, const VkAllocationCallbacks* allocator)
	{
		ImGui_Data* bd = ImGui_GetBackendData();
		if (bd->PipelineLayout)
			return;

		// Constants: we are using 'vec2 offset' and 'vec2 scale' instead of a full 3d projection matrix
		ImGui_CreateDescriptorSetLayout(device, allocator);
		VkPushConstantRange push_constants[1] = {};
		push_constants[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		push_constants[0].offset = sizeof(float) * 0;
		push_constants[0].size = sizeof(float) * 4;
		VkDescriptorSetLayout set_layout[1] = { bd->DescriptorSetLayout };
		VkPipelineLayoutCreateInfo layout_info = {};
		layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layout_info.setLayoutCount = 1;
		layout_info.pSetLayouts = set_layout;
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges = push_constants;
		VkResult  err = vkCreatePipelineLayout(*device, &layout_info, allocator, &bd->PipelineLayout);
		check_vk_result(err);
	}

	static void ImGui_CreatePipeline(const VkAllocationCallbacks* allocator, VkSampleCountFlagBits MSAASamples, VkPipeline* pipeline, uint32_t subpass)
	{
		auto pDevice = VulkanApi::GetInstance()->GetMainDevice();

		ImGui_Data* bd = ImGui_GetBackendData();
		ImGui_CreateShaderModules(pDevice, allocator);

		VkPipelineShaderStageCreateInfo stage[2] = {};
		stage[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stage[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stage[0].module = bd->ShaderModuleVert;
		stage[0].pName = "main";
		stage[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stage[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stage[1].module = bd->ShaderModuleFrag;
		stage[1].pName = "main";

		VkVertexInputBindingDescription binding_desc[1] = {};
		binding_desc[0].stride = sizeof(ImDrawVert);
		binding_desc[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		VkVertexInputAttributeDescription attribute_desc[3] = {};
		attribute_desc[0].location = 0;
		attribute_desc[0].binding = binding_desc[0].binding;
		attribute_desc[0].format = VK_FORMAT_R32G32_SFLOAT;
		attribute_desc[0].offset = IM_OFFSETOF(ImDrawVert, pos);
		attribute_desc[1].location = 1;
		attribute_desc[1].binding = binding_desc[0].binding;
		attribute_desc[1].format = VK_FORMAT_R32G32_SFLOAT;
		attribute_desc[1].offset = IM_OFFSETOF(ImDrawVert, uv);
		attribute_desc[2].location = 2;
		attribute_desc[2].binding = binding_desc[0].binding;
		attribute_desc[2].format = VK_FORMAT_R8G8B8A8_UNORM;
		attribute_desc[2].offset = IM_OFFSETOF(ImDrawVert, col);

		VkPipelineVertexInputStateCreateInfo vertex_info = {};
		vertex_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertex_info.vertexBindingDescriptionCount = 1;
		vertex_info.pVertexBindingDescriptions = binding_desc;
		vertex_info.vertexAttributeDescriptionCount = 3;
		vertex_info.pVertexAttributeDescriptions = attribute_desc;

		VkPipelineInputAssemblyStateCreateInfo ia_info = {};
		ia_info.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		ia_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo viewport_info = {};
		viewport_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewport_info.viewportCount = 1;
		viewport_info.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo raster_info = {};
		raster_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster_info.polygonMode = VK_POLYGON_MODE_FILL;
		raster_info.cullMode = VK_CULL_MODE_NONE;
		raster_info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster_info.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo ms_info = {};
		ms_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		ms_info.rasterizationSamples = (MSAASamples != 0) ? MSAASamples : VK_SAMPLE_COUNT_1_BIT;

		VkPipelineColorBlendAttachmentState color_attachment[1] = {};
		color_attachment[0].blendEnable = VK_TRUE;
		color_attachment[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		color_attachment[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		color_attachment[0].colorBlendOp = VK_BLEND_OP_ADD;
		color_attachment[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		color_attachment[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		color_attachment[0].alphaBlendOp = VK_BLEND_OP_ADD;
		color_attachment[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

		VkPipelineDepthStencilStateCreateInfo depth_info = {};
		depth_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

		VkPipelineColorBlendStateCreateInfo blend_info = {};
		blend_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		blend_info.attachmentCount = 1;
		blend_info.pAttachments = color_attachment;

		VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamic_state = {};
		dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamic_state.dynamicStateCount = (uint32_t)IM_ARRAYSIZE(dynamic_states);
		dynamic_state.pDynamicStates = dynamic_states;

		ImGui_CreatePipelineLayout(pDevice, allocator);

		VkFormat colorFormat = pDevice->GetColorFormat();
		VkFormat depthFormat = pDevice->GetDepthFormat();

		VkPipelineRenderingCreateInfoKHR dynamicRenderingExtension{};
		dynamicRenderingExtension.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
		dynamicRenderingExtension.colorAttachmentCount = 1;
		dynamicRenderingExtension.pColorAttachmentFormats = &colorFormat;
		dynamicRenderingExtension.depthAttachmentFormat = depthFormat;
		dynamicRenderingExtension.stencilAttachmentFormat = depthFormat;
		dynamicRenderingExtension.pNext = VK_NULL_HANDLE;

		// We're using dynamic rendeing extension
		VkGraphicsPipelineCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		info.flags = bd->PipelineCreateFlags;
		info.stageCount = 2;
		info.pStages = stage;
		info.pVertexInputState = &vertex_info;
		info.pInputAssemblyState = &ia_info;
		info.pViewportState = &viewport_info;
		info.pRasterizationState = &raster_info;
		info.pMultisampleState = &ms_info;
		info.pDepthStencilState = &depth_info;
		info.pColorBlendState = &blend_info;
		info.pDynamicState = &dynamic_state;
		info.layout = bd->PipelineLayout;
		info.renderPass = VK_NULL_HANDLE;
		info.subpass = subpass;
		info.pNext = &dynamicRenderingExtension;
		VkResult err = vkCreateGraphicsPipelines(*pDevice, VK_NULL_HANDLE, 1, &info, allocator, pipeline);
		check_vk_result(err);
	}

	bool ImGui_CreateDeviceObjects()
	{
		ImGui_Data* bd = ImGui_GetBackendData();
		ImGui_InitInfo* v = &bd->VulkanInitInfo;
		VkResult err;

		if (!bd->FontSampler)
		{
			VkSamplerCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			info.magFilter = VK_FILTER_LINEAR;
			info.minFilter = VK_FILTER_LINEAR;
			info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			info.minLod = -1000;
			info.maxLod = 1000;
			info.maxAnisotropy = 1.0f;
			err = vkCreateSampler(*v->Device, &info, v->Allocator, &bd->FontSampler);
			check_vk_result(err);
		}

		if (!bd->DescriptorSetLayout)
		{
			VkSampler sampler[1] = { bd->FontSampler };
			VkDescriptorSetLayoutBinding binding[1] = {};
			binding[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			binding[0].descriptorCount = 1;
			binding[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			binding[0].pImmutableSamplers = sampler;
			VkDescriptorSetLayoutCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			info.bindingCount = 1;
			info.pBindings = binding;
			err = vkCreateDescriptorSetLayout(*v->Device, &info, v->Allocator, &bd->DescriptorSetLayout);
			check_vk_result(err);
		}

		if (!bd->PipelineLayout)
		{
			// Constants: we are using 'vec2 offset' and 'vec2 scale' instead of a full 3d projection matrix
			VkPushConstantRange push_constants[1] = {};
			push_constants[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
			push_constants[0].offset = sizeof(float) * 0;
			push_constants[0].size = sizeof(float) * 4;
			VkDescriptorSetLayout set_layout[1] = { bd->DescriptorSetLayout };
			VkPipelineLayoutCreateInfo layout_info = {};
			layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			layout_info.setLayoutCount = 1;
			layout_info.pSetLayouts = set_layout;
			layout_info.pushConstantRangeCount = 1;
			layout_info.pPushConstantRanges = push_constants;
			err = vkCreatePipelineLayout(*v->Device, &layout_info, v->Allocator, &bd->PipelineLayout);
			check_vk_result(err);
		}

		ImGui_CreatePipeline(v->Allocator, v->MSAASamples, &bd->Pipeline, bd->Subpass);

		return true;
	}

	void    ImGui_DestroyFontUploadObjects()
	{
		ImGui_Data* bd = ImGui_GetBackendData();
		ImGui_InitInfo* v = &bd->VulkanInitInfo;
		if (bd->UploadBuffer)
		{
			vkDestroyBuffer(*v->Device, bd->UploadBuffer, v->Allocator);
			bd->UploadBuffer = VK_NULL_HANDLE;
		}
		if (bd->UploadBufferMemory)
		{
			vkFreeMemory(*v->Device, bd->UploadBufferMemory, v->Allocator);
			bd->UploadBufferMemory = VK_NULL_HANDLE;
		}
	}

	void    ImGui_DestroyDeviceObjects()
	{
		ImGui_Data* bd = ImGui_GetBackendData();
		ImGui_InitInfo* v = &bd->VulkanInitInfo;
		ImGuiH_DestroyWindowRenderBuffers(v->Device, &bd->MainWindowRenderBuffers, v->Allocator);
		ImGui_DestroyFontUploadObjects();

		if (bd->ShaderModuleVert) { vkDestroyShaderModule(*v->Device, bd->ShaderModuleVert, v->Allocator); bd->ShaderModuleVert = VK_NULL_HANDLE; }
		if (bd->ShaderModuleFrag) { vkDestroyShaderModule(*v->Device, bd->ShaderModuleFrag, v->Allocator); bd->ShaderModuleFrag = VK_NULL_HANDLE; }
		if (bd->FontView) { vkDestroyImageView(*v->Device, bd->FontView, v->Allocator); bd->FontView = VK_NULL_HANDLE; }
		if (bd->FontImage) { vkDestroyImage(*v->Device, bd->FontImage, v->Allocator); bd->FontImage = VK_NULL_HANDLE; }
		if (bd->FontMemory) { vkFreeMemory(*v->Device, bd->FontMemory, v->Allocator); bd->FontMemory = VK_NULL_HANDLE; }
		if (bd->FontSampler) { vkDestroySampler(*v->Device, bd->FontSampler, v->Allocator); bd->FontSampler = VK_NULL_HANDLE; }
		if (bd->DescriptorSetLayout) { vkDestroyDescriptorSetLayout(*v->Device, bd->DescriptorSetLayout, v->Allocator); bd->DescriptorSetLayout = VK_NULL_HANDLE; }
		if (bd->PipelineLayout) { vkDestroyPipelineLayout(*v->Device, bd->PipelineLayout, v->Allocator); bd->PipelineLayout = VK_NULL_HANDLE; }
		if (bd->Pipeline) { vkDestroyPipeline(*v->Device, bd->Pipeline, v->Allocator); bd->Pipeline = VK_NULL_HANDLE; }
	}

	bool    ImGui_Init(ImGui_InitInfo* info, VkRenderPass render_pass)
	{
		ImGuiIO& io = ImGui::GetIO();
		IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");

		// Setup backend capabilities flags
		ImGui_Data* bd = IM_NEW(ImGui_Data)();
		io.BackendRendererUserData = (void*)bd;
		io.BackendRendererName = "imgui_impl_vulkan";
		io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.

		IM_ASSERT(info->Instance != VK_NULL_HANDLE);
		IM_ASSERT(info->Device != VK_NULL_HANDLE);
		IM_ASSERT(info->DescriptorPool != VK_NULL_HANDLE);
		IM_ASSERT(info->MinImageCount >= 2);
		IM_ASSERT(info->ImageCount >= info->MinImageCount);
		IM_ASSERT(render_pass != VK_NULL_HANDLE);

		bd->VulkanInitInfo = *info;
		bd->RenderPass = render_pass;
		bd->Subpass = info->Subpass;

		ImGui_CreateDeviceObjects();

		return true;
	}

	void ImGui_Shutdown()
	{
		ImGui_Data* bd = ImGui_GetBackendData();
		IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
		ImGuiIO& io = ImGui::GetIO();

		ImGui_DestroyDeviceObjects();
		io.BackendRendererName = nullptr;
		io.BackendRendererUserData = nullptr;
		IM_DELETE(bd);
	}

	void ImGui_NewFrame()
	{
		ImGui_Data* bd = ImGui_GetBackendData();
		IM_ASSERT(bd != nullptr && "Did you call ImGui_Init()?");
		IM_UNUSED(bd);
	}

	void ImGui_SetMinImageCount(uint32_t min_image_count)
	{
		ImGui_Data* bd = ImGui_GetBackendData();
		IM_ASSERT(min_image_count >= 2);
		if (bd->VulkanInitInfo.MinImageCount == min_image_count)
			return;

		ImGui_InitInfo* v = &bd->VulkanInitInfo;
		VkResult err = vkDeviceWaitIdle(*v->Device);
		check_vk_result(err);
		ImGuiH_DestroyWindowRenderBuffers(v->Device, &bd->MainWindowRenderBuffers, v->Allocator);
		bd->VulkanInitInfo.MinImageCount = min_image_count;
	}

	// Register a texture
	// FIXME: This is experimental in the sense that we are unsure how to best design/tackle this problem, please post to https://github.com/ocornut/imgui/pull/914 if you have suggestions.
	VkDescriptorSet ImGui_AddTexture(VkSampler sampler, VkImageView image_view, VkImageLayout image_layout)
	{
		ImGui_Data* bd = ImGui_GetBackendData();
		ImGui_InitInfo* v = &bd->VulkanInitInfo;

		// Create Descriptor Set:
		VkDescriptorSet descriptor_set;
		{
			VkDescriptorSetAllocateInfo alloc_info = {};
			alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			alloc_info.descriptorPool = v->DescriptorPool;
			alloc_info.descriptorSetCount = 1;
			alloc_info.pSetLayouts = &bd->DescriptorSetLayout;
			VkResult err = vkAllocateDescriptorSets(*v->Device, &alloc_info, &descriptor_set);
			check_vk_result(err);
		}

		// Update the Descriptor Set:
		{
			VkDescriptorImageInfo desc_image[1] = {};
			desc_image[0].sampler = sampler;
			desc_image[0].imageView = image_view;
			desc_image[0].imageLayout = image_layout;
			VkWriteDescriptorSet write_desc[1] = {};
			write_desc[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write_desc[0].dstSet = descriptor_set;
			write_desc[0].descriptorCount = 1;
			write_desc[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write_desc[0].pImageInfo = desc_image;
			vkUpdateDescriptorSets(*v->Device, 1, write_desc, 0, nullptr);
		}
		return descriptor_set;
	}

	void ImGui_RemoveTexture(VkDescriptorSet descriptor_set)
	{
		ImGui_Data* bd = ImGui_GetBackendData();
		ImGui_InitInfo* v = &bd->VulkanInitInfo;
		vkFreeDescriptorSets(*v->Device, v->DescriptorPool, 1, &descriptor_set);
	}

	void ImGuiH_DestroyFrameRenderBuffers(VulkanDevicePtr device, ImGuiH_FrameRenderBuffers* buffers, const VkAllocationCallbacks* allocator)
	{
		buffers->VertexBuffer.Clear();
		buffers->IndexBuffer.Clear();
		buffers->VertexBufferSize = 0;
		buffers->IndexBufferSize = 0;
	}

	void ImGuiH_DestroyWindowRenderBuffers(VulkanDevicePtr device, ImGuiH_WindowRenderBuffers* buffers, const VkAllocationCallbacks* allocator)
	{
		for (uint32_t n = 0; n < buffers->Count; n++)
			ImGuiH_DestroyFrameRenderBuffers(device, &buffers->FrameRenderBuffers[n], allocator);
		IM_FREE(buffers->FrameRenderBuffers);
		buffers->FrameRenderBuffers = nullptr;
		buffers->Index = 0;
		buffers->Count = 0;
	}
}