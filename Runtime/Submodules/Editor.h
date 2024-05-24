#pragma once
#include "Core/Submodule.h"

namespace Sailor
{
	class Editor : public TSubmodule<Editor>
	{
	public:

		Editor(HWND editorHwnd, uint32_t editorPort);

	protected:

		uint32_t m_editorPort;
		HWND m_editorHwnd;
	};
}