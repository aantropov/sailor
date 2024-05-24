#pragma once
#include "Core/Submodule.h"
#include <concurrent_queue.h>
#include <wtypes.h>

namespace Sailor
{
	class Editor : public TSubmodule<Editor>
	{
	public:

		Editor(HWND editorHwnd, uint32_t editorPort);

		void PushMessage(const std::string& msg);
		bool PullMessage(std::string& msg);
		__forceinline size_t NumMessages() const { return m_messages.unsafe_size(); }

	protected:

		concurrency::concurrent_queue<std::string> m_messages;

		uint32_t m_editorPort;
		HWND m_editorHwnd;
	};
}	