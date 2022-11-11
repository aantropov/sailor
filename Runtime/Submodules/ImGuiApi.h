#pragma once
#include "Core/Submodule.h"
#include "RHI/Types.h"
#include "imgui/imgui.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include <wtypes.h>

namespace Sailor
{
	class ImGuiApi : public TSubmodule<ImGuiApi>
	{
	public:

		ImGuiApi(void* hWnd);
		virtual ~ImGuiApi();

		void NewFrame();
		void RenderFrame(RHI::RHICommandListPtr transferCmdList, RHI::RHICommandListPtr drawCmdList);
		void HandleWin32(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

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
		SAILOR_API static void ImGui_RenderDrawData(ImDrawData* draw_data, RHI::RHICommandListPtr transferCmdList, RHI::RHICommandListPtr drawCmdList);
		SAILOR_API static void ImGui_SetMinImageCount(uint32_t min_image_count); 
		SAILOR_API static void ImGui_SetupRenderState(ImDrawData* drawData, RHI::RHICommandListPtr cmdList, FrameRenderBuffers* rb, int width, int height);
		SAILOR_API static void CreateOrResizeBuffer(RHI::RHIBufferPtr& buffer, size_t newSize);
		SAILOR_API static void DestroyWindowRenderBuffers(WindowRenderBuffers* buffers);
	};
}