#pragma once
#include <utility>

namespace Sailor::Tests
{
	template<typename TAction>
	class ScopeExit
	{
	public:
		explicit ScopeExit(TAction action) : m_action(std::move(action)) {}
		ScopeExit(const ScopeExit&) = delete;
		ScopeExit& operator=(const ScopeExit&) = delete;
		~ScopeExit() { Run(); }

		void Run()
		{
			if (std::exchange(m_bIsPending, false)) m_action();
		}

	private:
		TAction m_action;
		bool m_bIsPending = true;
	};
}
