#include "ImGuiApi.h"

#include <stdio.h>
#include <imgui.h>
#include <ImGuizmo.h>
#if defined(_WIN32)
#include <imgui_impl_win32.h>
#endif
#include "Core/LogMacros.h"
#include "Tasks/Scheduler.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Platform/Win32/Input.h"

#include "Memory/MemoryBlockAllocator.hpp"
#include "RHI/Renderer.h"
#include "RHI/GraphicsDriver.h"
#include "RHI/CommandList.h"
#include "RHI/VertexDescription.h"
#include "Sailor.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

using namespace Sailor;

ImGuiContext* ImGuiApi::GetCurrentContext()
{
	return ImGui::GetCurrentContext();
}

void ImGuiApi::GetAllocatorFunctions(ImGuiMemAllocFunc* alloc, ImGuiMemFreeFunc* free, void** userData)
{
	ImGui::GetAllocatorFunctions(alloc, free, userData);
}

namespace
{
	// COUNT represents no native cursor override; None hides the native cursor.
	std::atomic<ImGuiMouseCursor> g_requestedMouseCursor{ ImGuiMouseCursor_COUNT };

	// Native adapters and the editor protocol use the same virtual-key codes.
	ImGuiKey MapInputKey(uint32_t key)
	{
		if (key >= '0' && key <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + key - '0');
		if (key >= 'A' && key <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + key - 'A');
		if (key >= 0x70 && key <= 0x87) return static_cast<ImGuiKey>(ImGuiKey_F1 + key - 0x70);
		if (key >= 0x60 && key <= 0x69) return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + key - 0x60);
		switch (key)
		{
		case 0x08: return ImGuiKey_Backspace;
		case 0x09: return ImGuiKey_Tab;
		case 0x0D: return ImGuiKey_Enter;
		case 0x13: return ImGuiKey_Pause;
		case 0x14: return ImGuiKey_CapsLock;
		case 0x1B: return ImGuiKey_Escape;
		case 0x20: return ImGuiKey_Space;
		case 0x21: return ImGuiKey_PageUp;
		case 0x22: return ImGuiKey_PageDown;
		case 0x23: return ImGuiKey_End;
		case 0x24: return ImGuiKey_Home;
		case 0x25: return ImGuiKey_LeftArrow;
		case 0x26: return ImGuiKey_UpArrow;
		case 0x27: return ImGuiKey_RightArrow;
		case 0x28: return ImGuiKey_DownArrow;
		case 0x2C: return ImGuiKey_PrintScreen;
		case 0x2D: return ImGuiKey_Insert;
		case 0x2E: return ImGuiKey_Delete;
		case 0x5B: return ImGuiKey_LeftSuper;
		case 0x5C: return ImGuiKey_RightSuper;
		case 0x5D: return ImGuiKey_Menu;
		case 0x6A: return ImGuiKey_KeypadMultiply;
		case 0x6B: return ImGuiKey_KeypadAdd;
		case 0x6D: return ImGuiKey_KeypadSubtract;
		case 0x6E: return ImGuiKey_KeypadDecimal;
		case 0x6F: return ImGuiKey_KeypadDivide;
		case 0x90: return ImGuiKey_NumLock;
		case 0x91: return ImGuiKey_ScrollLock;
		case 0xA0: return ImGuiKey_LeftShift;
		case 0xA1: return ImGuiKey_RightShift;
		case 0xA2: return ImGuiKey_LeftCtrl;
		case 0xA3: return ImGuiKey_RightCtrl;
		case 0xA4: return ImGuiKey_LeftAlt;
		case 0xA5: return ImGuiKey_RightAlt;
		case 0xBA: return ImGuiKey_Semicolon;
		case 0xBB: return ImGuiKey_Equal;
		case 0xBC: return ImGuiKey_Comma;
		case 0xBD: return ImGuiKey_Minus;
		case 0xBE: return ImGuiKey_Period;
		case 0xBF: return ImGuiKey_Slash;
		case 0xC0: return ImGuiKey_GraveAccent;
		case 0xDB: return ImGuiKey_LeftBracket;
		case 0xDC: return ImGuiKey_Backslash;
		case 0xE2: return ImGuiKey_Backslash;
		case 0xDD: return ImGuiKey_RightBracket;
		case 0xDE: return ImGuiKey_Apostrophe;
		default: return ImGuiKey_None;
		}
	}
}

std::optional<ImGuiMouseCursor> ImGuiApi::GetRequestedMouseCursor()
{
	const auto cursor = g_requestedMouseCursor.load(std::memory_order_relaxed);
	if (cursor == ImGuiMouseCursor_COUNT) return std::nullopt;
	return cursor;
}

void ImGuiApi::HandleInput(const Platform::InputEvent& event)
{
	if (!ImGui::GetCurrentContext()) return;

	using Type = Platform::InputEvent::Type;
	ImGuiIO& io = ImGui::GetIO();
	if (event.m_type == Type::Reset || (event.m_type == Type::Focus && !event.m_bIsPressed))
	{
		io.ClearEventsQueue();
		io.ClearInputKeys();
		io.ClearInputMouse();
	}
	switch (event.m_type)
	{
	case Type::MousePos:
		io.AddMousePosEvent(event.m_x, event.m_y);
		break;
	case Type::MouseButton:
		io.AddMousePosEvent(event.m_x, event.m_y);
		if (event.m_button >= 0 && event.m_button < ImGuiMouseButton_COUNT)
			io.AddMouseButtonEvent(event.m_button, event.m_bIsPressed);
		break;
	case Type::MouseWheel:
		io.AddMouseWheelEvent(event.m_x, event.m_y);
		break;
	case Type::Key:
	{
		const auto& state = Win32::GlobalInput::GetInputState();
		io.AddKeyEvent(ImGuiKey_ModShift, state.IsKeyDown(VK_SHIFT));
		io.AddKeyEvent(ImGuiKey_ModCtrl, state.IsKeyDown(VK_CONTROL));
		io.AddKeyEvent(ImGuiKey_ModAlt, state.IsKeyDown(VK_MENU));
		io.AddKeyEvent(ImGuiKey_ModSuper, state.IsKeyDown(VK_LWIN) || state.IsKeyDown(VK_RWIN));
		const auto key = event.m_key == 0x0D && event.m_bIsKeypad ? ImGuiKey_KeypadEnter : MapInputKey(event.m_key);
		if (key != ImGuiKey_None)
			io.AddKeyEvent(key, event.m_bIsPressed);
		break;
	}
	case Type::Text:
		io.AddInputCharactersUTF8(event.m_text.c_str());
		break;
	case Type::CharacterUtf16:
		io.AddInputCharacterUTF16(static_cast<ImWchar16>(event.m_key));
		break;
	case Type::Focus:
		io.AddFocusEvent(event.m_bIsPressed);
		break;
	default:
		break;
	}
}

ImGuiApi::ImGuiApi(void* hWnd)
{
	SAILOR_PROFILE_FUNCTION();

	m_pContext = ImGui::CreateContext();
	ImGuizmo::SetImGuiContext(m_pContext);
#if defined(_WIN32)
	ImGui_ImplWin32_Init(hWnd);
#else
	(void)hWnd;
#endif

	InitInfo initInfo = {};
	// CPU preparation precedes flight acquisition by one frame. PushFrame
	// waits the oldest flight before another CPU frame can be prepared.
	initInfo.MinImageCount = std::max(2u, RHI::Renderer::GetDriver()->GetMaxFramesInFlight() + 1u);
	initInfo.ImageCount = initInfo.MinImageCount;

	ImGui_Init(&initInfo);
}

ImGuiApi::~ImGuiApi()
{
	SAILOR_PROFILE_FUNCTION();
	ImGui::SetCurrentContext(m_pContext);
	g_requestedMouseCursor.store(ImGuiMouseCursor_COUNT, std::memory_order_relaxed);
	m_preparedFrames.clear();
	m_textureBindings.clear();

#if defined(_WIN32)
	ImGui_ImplWin32_Shutdown();
#endif
	ImGuiApi::ImGui_Shutdown();
	ImGui::DestroyContext(m_pContext);
	ImGuizmo::SetImGuiContext(nullptr);
}

void ImGuiApi::NewFrame()
{
	SAILOR_PROFILE_FUNCTION();
	std::erase_if(m_preparedFrames, [](const PreparedFramePtr& frame)
		{
			return !frame.IsShared();
		});

	Data* bd = ImGui_GetBackendData();
	IM_ASSERT(bd != nullptr && "Did you call ImGui_Init()?");
	if (!bd->FontTexture) ImGui_CreateFontsTexture();

#if defined(_WIN32)
	if (!App::IsEditorMode())
	{
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		ImGuizmo::BeginFrame();
		return;
	}
#endif
	static auto s_prevFrameTime = std::chrono::steady_clock::now();
	const auto now = std::chrono::steady_clock::now();
	const float deltaTime = std::chrono::duration<float>(now - s_prevFrameTime).count();
	s_prevFrameTime = now;

	auto& window = App::GetMainWindow();
	ImGuiIO& io = ImGui::GetIO();
	if (window)
	{
		const float displayWidth = (float)window->GetWidth();
		const float displayHeight = (float)window->GetHeight();
		io.DisplaySize = ImVec2(displayWidth, displayHeight);

		const glm::ivec2 renderArea = window->GetRenderArea();
#if defined(_WIN32)
		io.DisplaySize = ImVec2(static_cast<float>(renderArea.x), static_cast<float>(renderArea.y));
		io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
#else
		if (displayWidth > 0.0f && displayHeight > 0.0f && renderArea.x > 0 && renderArea.y > 0)
		{
			io.DisplayFramebufferScale = ImVec2((float)renderArea.x / displayWidth, (float)renderArea.y / displayHeight);
		}
		else
		{
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
		}
#endif
	}
	io.DeltaTime = deltaTime > 0.0f ? deltaTime : (1.0f / 60.0f);
	if (!window)
	{
		io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	}
	ImGui::NewFrame();
	ImGuizmo::BeginFrame();
}

ImGuiApi::PreparedFramePtr ImGuiApi::PrepareFrame(RHI::RHICommandListPtr transferCmdList)
{
	SAILOR_PROFILE_FUNCTION();
	ImGui::Render();
	const auto& io = ImGui::GetIO();
	const auto cursor = (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) ? ImGuiMouseCursor_COUNT
		: io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
	g_requestedMouseCursor.store(cursor, std::memory_order_relaxed);
	const Data* bd = ImGui_GetBackendData();
	if (!bd->FontTexture) return {};
	auto frame = TSharedPtr<PreparedFrame>::Make(ImGui::GetDrawData());
	frame->Material = bd->Material;
	frame->ShaderBindings = bd->ShaderBindings;
	// ImTextureID stores an RHITexture pointer. Retain each texture through its
	// bindings while this immutable draw snapshot is in flight on the GPU.
	frame->TextureBindings.emplace((ImTextureID)bd->FontTexture.GetRawPtr(), bd->ShaderBindings);
	const auto& drawData = frame->DrawData.GetDrawData();
	for (int list = 0; list < drawData.CmdListsCount; ++list)
	{
		for (const auto& command : drawData.CmdLists[list]->CmdBuffer)
		{
			const auto textureId = command.GetTexID();
			if (!textureId || command.UserCallback || frame->TextureBindings.contains(textureId)) continue;
			if (const auto cached = m_textureBindings.find(textureId); cached != m_textureBindings.end())
			{
				frame->TextureBindings.emplace(textureId, cached->second);
				continue;
			}
			auto texture = reinterpret_cast<RHI::RHITexture*>(textureId)->ToRefPtr<RHI::RHITexture>();
			auto bindings = RHI::Renderer::GetDriver()->CreateShaderBindings();
			RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(bindings, "sTexture"_h, texture, 0u);
			frame->TextureBindings.emplace(textureId, std::move(bindings));
		}
	}
	m_textureBindings = frame->TextureBindings;
	ImGui_UpdateDrawData(*frame, transferCmdList);
	m_preparedFrames.push_back(frame);
	return frame;
}

void ImGuiApi::RenderFrame(const PreparedFramePtr& frame, RHI::RHICommandListPtr drawCmdList)
{
	SAILOR_PROFILE_FUNCTION();
	if (frame)
	{
		ImGui_RenderDrawData(*frame, drawCmdList);
	}
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

void ImGuiApi::ImGui_SetupRenderState(const PreparedFrame& frame, RHI::RHICommandListPtr cmdList, int width, int height)
{
	SAILOR_PROFILE_FUNCTION();
	const ImDrawData* drawData = &frame.DrawData.GetDrawData();

	RHI::Renderer::GetDriverCommands()->BindMaterial(cmdList, frame.Material);

	// Bind Vertex And Index Buffer:
	if (drawData->TotalVtxCount > 0)
	{
		const bool bUint16InsteadOfUint32 = sizeof(ImDrawIdx) == 2;
		RHI::Renderer::GetDriverCommands()->BindVertexBuffer(cmdList, frame.VertexBuffer, 0);
		RHI::Renderer::GetDriverCommands()->BindIndexBuffer(cmdList, frame.IndexBuffer, 0, bUint16InsteadOfUint32);
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

		RHI::Renderer::GetDriverCommands()->PushConstants(cmdList, frame.Material, sizeof(float) * 4, pushConstants);
	}
}

void ImGuiApi::ImGui_UpdateDrawData(PreparedFrame& frame, RHI::RHICommandListPtr transferCmdList)
{
	SAILOR_PROFILE_FUNCTION();
	const ImDrawData* drawData = &frame.DrawData.GetDrawData();

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
		wrb->FrameRenderBuffers = new FrameRenderBuffers[wrb->Count]{};
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
		frame.VertexBuffer = rb->VertexBuffer;
		frame.IndexBuffer = rb->IndexBuffer;

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

void ImGuiApi::ImGui_RenderDrawData(const PreparedFrame& frame, RHI::RHICommandListPtr drawCmdList)
{
	SAILOR_PROFILE_FUNCTION();
	const ImDrawData* drawData = &frame.DrawData.GetDrawData();

	if (!drawData->Valid)
		return;

	// Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
	const int width = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
	const int height = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);

	if (width <= 0 || height <= 0)
		return;

	ImGui_SetupRenderState(frame, drawCmdList, width, height);

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
					ImGui_SetupRenderState(frame, drawCmdList, width, height);
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

				const int32_t clipMinX = (int32_t)std::floor(clipMin.x);
				const int32_t clipMinY = (int32_t)std::floor(clipMin.y);
				const int32_t clipMaxX = (int32_t)std::ceil(clipMax.x);
				const int32_t clipMaxY = (int32_t)std::ceil(clipMax.y);

				RHI::Renderer::GetDriverCommands()->SetViewport(drawCmdList,
					0, 0,
					(float)width, (float)height,
					glm::ivec2(clipMinX, clipMinY),
					glm::ivec2(clipMaxX - clipMinX, clipMaxY - clipMinY),
					0, 1.0f);

				const auto texture = frame.TextureBindings.find(pcmd->GetTexID());
				const auto& bindings = texture != frame.TextureBindings.end() ? texture->second : frame.ShaderBindings;
				if (!RHI::Renderer::GetDriverCommands()->BindShaderBindings(drawCmdList, frame.Material, { bindings }))
				{
					continue;
				}
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

void ImGuiApi::ImGui_CreateFontsTexture()
{
	ImGuiIO& io = ImGui::GetIO();
	Data* bd = ImGui_GetBackendData();
	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	auto texture = RHI::Renderer::GetDriver()->CreateTexture(pixels,
		size_t(width) * height * 4,
		glm::ivec3(width, height, 1),
		1,
		RHI::ETextureType::Texture2D,
		RHI::ETextureFormat::R8G8B8A8_UNORM,
		RHI::ETextureFiltration::Linear,
		RHI::ETextureClamping::Repeat,
		RHI::ETextureUsageBit::Sampled_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit);
	if (!texture) return;
	if (!RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(bd->ShaderBindings, "sTexture"_h, texture, 0)) return;
	bd->FontTexture = std::move(texture);
	io.Fonts->SetTexID((ImTextureID)(bd->FontTexture.GetRawPtr()));
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

	bd->ShaderBindings = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
	ImGui_CreateFontsTexture();

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
	delete[] buffers->FrameRenderBuffers;
	buffers->FrameRenderBuffers = nullptr;
	buffers->Index = 0;
	buffers->Count = 0;
}
