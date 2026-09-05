#pragma once
#include "Core/Submodule.h"
#include "RHI/Types.h"
#include <imgui.h>
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "ImGuiDrawDataSnapshot.h"
#include <cstdint>
#include <memory>
#include <vector>

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
		};
		using PreparedFramePtr = std::shared_ptr<const PreparedFrame>;

		ImGuiApi(void* hWnd);
		virtual ~ImGuiApi();

		void NewFrame();
		PreparedFramePtr PrepareFrame(RHI::RHICommandListPtr transferCmdList);
		static void RenderFrame(const PreparedFramePtr& frame, RHI::RHICommandListPtr drawCmdList);
#if defined(_WIN32)
		void HandleWin32(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

		struct WindowsEditorInputEvent
		{
			enum class Type : uint8_t
			{
				MousePos,
				MouseButton,
				MouseWheel,
				Key,
				Focus
			};

			Type EventType = Type::MousePos;
			float X = 0.0f;
			float Y = 0.0f;
			uint32_t Key = 0;
			int32_t Button = -1;
			bool bPressed = false;
		};

		void HandleWindowsEditorInput(const WindowsEditorInputEvent& event);
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

		// Keep the final owner on the CPU thread: ImGui allocation accounting is
		// context-owned too. RHI tasks only release their shared references.
		std::vector<PreparedFramePtr> m_preparedFrames;

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
		SAILOR_API static void ImGui_UpdateDrawData(PreparedFrame& frame, RHI::RHICommandListPtr transferCmdList);
		SAILOR_API static void ImGui_RenderDrawData(const PreparedFrame& frame, RHI::RHICommandListPtr drawCmdList);
		SAILOR_API static void ImGui_SetMinImageCount(uint32_t min_image_count);
		SAILOR_API static void ImGui_SetupRenderState(const PreparedFrame& frame, RHI::RHICommandListPtr cmdList, int width, int height);
		SAILOR_API static void CreateOrResizeBuffer(RHI::RHIBufferPtr& buffer, size_t newSize);
		SAILOR_API static void DestroyWindowRenderBuffers(WindowRenderBuffers* buffers);
	};
}
