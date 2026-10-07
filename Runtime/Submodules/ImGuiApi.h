#pragma once
#include "Core/Submodule.h"
#include "Platform/InputEvent.h"
#include "RHI/Types.h"
#include <imgui.h>
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "ImGuiDrawDataSnapshot.h"
#include "Memory/SharedPtr.hpp"
#include <cstdint>
#include <optional>
#include <vector>
#include <unordered_map>

namespace Sailor
{
	class ImGuiApi : public TSubmodule<ImGuiApi>
	{
	public:

		struct PreparedFrame
		{
			explicit PreparedFrame(const ImDrawData* data) : DrawData(data) {}

			ImGuiDrawDataSnapshot DrawData;
			RHI::RHIBufferPtr VertexBuffer;
			RHI::RHIBufferPtr IndexBuffer;
			RHI::RHIMaterialPtr Material;
			RHI::RHIShaderBindingSetPtr ShaderBindings;
			std::unordered_map<ImTextureID, RHI::RHIShaderBindingSetPtr> TextureBindings;
		};
		using PreparedFramePtr = TSharedPtr<const PreparedFrame>;

		ImGuiApi(void* hWnd);
		virtual ~ImGuiApi();

		// Workspace DLLs statically link ImGui and must bind the engine's context
		// and allocators before issuing UI commands on the CPU frame thread.
		SAILOR_SHARED_API static ImGuiContext* GetCurrentContext();
		SAILOR_SHARED_API static void GetAllocatorFunctions(ImGuiMemAllocFunc* alloc,
			ImGuiMemFreeFunc* free, void** userData);

		SAILOR_SHARED_API void NewFrame();
		SAILOR_SHARED_API PreparedFramePtr PrepareFrame(RHI::RHICommandListPtr transferCmdList);
		static void RenderFrame(const PreparedFramePtr& frame, RHI::RHICommandListPtr drawCmdList);
		SAILOR_API static void HandleInput(const Platform::InputEvent& event);
		// A native UI thread reads only the last prepared cursor, never the ImGui context.
		SAILOR_SHARED_API static std::optional<ImGuiMouseCursor> GetRequestedMouseCursor();

	protected:

		ImGuiContext* m_pContext = nullptr;

		// Keep the final owner on the CPU thread: ImGui allocation accounting is
		// context-owned too. RHI tasks only release their shared references.
		std::vector<PreparedFramePtr> m_preparedFrames;
		std::unordered_map<ImTextureID, RHI::RHIShaderBindingSetPtr> m_textureBindings;

		struct InitInfo
		{
			uint32_t                        Subpass{};
			uint32_t                        MinImageCount{}; // >= 2
			uint32_t                        ImageCount{};    // >= MinImageCount
		};

		struct FrameRenderBuffers
		{
			RHI::RHIBufferPtr   VertexBuffer;
			RHI::RHIBufferPtr   IndexBuffer;
		};

		struct WindowRenderBuffers
		{
			uint32_t            Index;
			uint32_t            Count;
			FrameRenderBuffers* FrameRenderBuffers;
		};

		struct Data
		{
			InitInfo				    InitInfo;
			size_t                      BufferMemoryAlignment;
			RHI::RHIMaterialPtr         Material;
			uint32_t                    Subpass;
			ShaderSetPtr                Shader;
			RHI::RHIShaderBindingSetPtr ShaderBindings;
			RHI::RHITexturePtr			FontTexture;
			WindowRenderBuffers MainWindowRenderBuffers;

			Data() { memset((void*)this, 0, sizeof(*this)); BufferMemoryAlignment = 256; }
		};

		// Forward Declarations
		SAILOR_API static Data* ImGui_GetBackendData();
		SAILOR_API static bool ImGui_Init(InitInfo* info);
		static void ImGui_CreateFontsTexture();
		SAILOR_API static void ImGui_Shutdown();
		SAILOR_API static void ImGui_UpdateDrawData(PreparedFrame& frame, RHI::RHICommandListPtr transferCmdList);
		SAILOR_API static void ImGui_RenderDrawData(const PreparedFrame& frame, RHI::RHICommandListPtr drawCmdList);
		SAILOR_API static void ImGui_SetMinImageCount(uint32_t min_image_count);
		SAILOR_API static void ImGui_SetupRenderState(const PreparedFrame& frame, RHI::RHICommandListPtr cmdList, int width, int height);
		SAILOR_API static void CreateOrResizeBuffer(RHI::RHIBufferPtr& buffer, size_t newSize);
		SAILOR_API static void DestroyWindowRenderBuffers(WindowRenderBuffers* buffers);
	};
}
