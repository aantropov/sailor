#pragma once
#include "Core/Submodule.h"
#include "RHI/Types.h"
#include "imgui/imgui.h"

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
	};
}