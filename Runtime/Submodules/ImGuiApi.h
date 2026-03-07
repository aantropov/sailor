#pragma once
#include "Core/Submodule.h"
#include "RHI/Types.h"
#include <imgui.h>
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include <cstdint>

namespace Sailor
{
	class ImGuiApi : public TSubmodule<ImGuiApi>
	{
	public:

		ImGuiApi(void* hWnd);
		virtual ~ImGuiApi();

		void NewFrame();
		void PrepareFrame(RHI::RHICommandListPtr transferCmdList);
		void RenderFrame(RHI::RHICommandListPtr drawCmdList);
#if defined(_WIN32)
		void HandleWin32(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#elif defined(__APPLE__)
		struct MacEvent
		{
			enum class Type : uint8_t
			{
				MousePos,
				MouseButton,
				MouseWheel,
				Key,
				Text,
				Focus
			};

			Type EventType = Type::MousePos;
			float X = 0.0f;
			float Y = 0.0f;
			uint32_t Key = 0;
			int32_t Button = -1;
			bool bPressed = false;
			const char* TextUtf8 = nullptr;
		};

		void HandleMac(const MacEvent& event);
#endif

	protected:

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
		SAILOR_API static void ImGui_Shutdown();
		SAILOR_API static void ImGui_UpdateDrawData(ImDrawData* draw_data, RHI::RHICommandListPtr transferCmdList);
		SAILOR_API static void ImGui_RenderDrawData(ImDrawData* draw_data, RHI::RHICommandListPtr drawCmdList);
		SAILOR_API static void ImGui_SetMinImageCount(uint32_t min_image_count);
		SAILOR_API static void ImGui_SetupRenderState(ImDrawData* drawData, RHI::RHICommandListPtr cmdList, const FrameRenderBuffers* rb, int width, int height);
		SAILOR_API static void CreateOrResizeBuffer(RHI::RHIBufferPtr& buffer, size_t newSize);
		SAILOR_API static void DestroyWindowRenderBuffers(WindowRenderBuffers* buffers);
	};
}
