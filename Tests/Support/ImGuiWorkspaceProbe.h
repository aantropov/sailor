#pragma once

#include <atomic>
#include <cstdint>

namespace Sailor::Tests
{
	struct ImGuiWorkspaceProbe
	{
		std::atomic<uint32_t> m_frames{ 0 };
		std::atomic<uint32_t> m_callbacksStarted{ 0 };
		std::atomic<uint32_t> m_callbacksFinished{ 0 };
		std::atomic<uint32_t> m_copiedCallbacks{ 0 };
		std::atomic<uint32_t> m_borrowedCallbacks{ 0 };
		std::atomic<bool> m_bIsCallbackReleased{ false };
		std::atomic<bool> m_bIsCallbackDataValid{ true };
		bool m_bHasPrivateContext = false;
		bool m_bWasComponentDestroyedWithContext = false;
		bool m_bWasComponentDestroyedAfterCallbacks = false;
		bool m_bWasModuleUnloaded = false;
		bool m_bWasUnloadedAfterContext = false;
	};
}
