#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace Sailor::Protocol
{
	class TEditorEngineProtocolLifecycleGate final
	{
	public:
		bool TryBeginInitialization(std::string& outError)
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			if (m_state != EState::Idle &&
				m_state != EState::ShutdownComplete)
			{
				outError = "Engine initialization has already been requested.";
				return false;
			}

			m_state = EState::Initializing;
			m_bInitializationActive = true;
			m_bStopRequested = false;
			m_condition.wait(lock, [this]()
				{
					return m_numActiveOperations == 0 && !m_bStartActive;
				});
			if (m_state != EState::Initializing)
			{
				m_bInitializationActive = false;
				outError = "Engine initialization was superseded by shutdown.";
				lock.unlock();
				m_condition.notify_all();
				return false;
			}
			return true;
		}

		void CompleteInitialization(const bool bSucceeded)
		{
			{
				const std::lock_guard<std::mutex> lock(m_mutex);
				m_bInitializationActive = false;
				if (m_state == EState::Initializing)
				{
					m_state = bSucceeded ? EState::Ready : EState::Idle;
				}
				m_bStartIssued = false;
				m_bStartActive = false;
				if (!bSucceeded && m_state != EState::ShuttingDown)
				{
					m_bStopRequested = false;
				}
			}
			m_condition.notify_all();
		}

		bool TryBeginStart(std::string& outError)
		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			if (m_state != EState::Ready)
			{
				outError = "Engine start requires a completed initialization.";
				return false;
			}
			if (m_bStartIssued || m_bStopRequested)
			{
				outError = "Engine start has already been requested for this session.";
				return false;
			}

			m_bStartIssued = true;
			m_bStartActive = true;
			return true;
		}

		void CompleteStart()
		{
			{
				const std::lock_guard<std::mutex> lock(m_mutex);
				m_bStartActive = false;
			}
			m_condition.notify_all();
		}

		bool NoteStopRequested()
		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			if (m_bInitializationActive)
			{
				// Initialization owns the partially built App state, so do not
				// enter App::Stop concurrently. Preserve the request so a late
				// Start for the new session is still rejected.
				m_bStopRequested = true;
				return false;
			}
			if (m_state == EState::Ready ||
				m_state == EState::ShuttingDown)
			{
				m_bStopRequested = true;
				return true;
			}
			return false;
		}

		bool TryBeginShutdown(std::string& outError)
		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			if (m_state == EState::ShuttingDown ||
				m_state == EState::ShutdownComplete)
			{
				outError = "Engine shutdown has already been requested.";
				return false;
			}

			m_state = EState::ShuttingDown;
			m_bStopRequested = true;
			return true;
		}

		void WaitForInitializationDrain()
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_condition.wait(lock, [this]()
				{
					return !m_bInitializationActive;
				});
		}

		void WaitForShutdownDrain()
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_condition.wait(lock, [this]()
				{
					return m_numActiveOperations == 0 &&
						!m_bInitializationActive &&
						!m_bStartActive;
				});
		}

		void CompleteShutdown()
		{
			{
				const std::lock_guard<std::mutex> lock(m_mutex);
				m_state = EState::ShutdownComplete;
				m_bInitializationActive = false;
				m_bStartIssued = false;
				m_bStartActive = false;
				m_bStopRequested = true;
			}
			m_condition.notify_all();
		}

		bool TryAcquireOperation(
			std::string& outError,
			const bool bAllowWhenIdle)
		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			if (m_state == EState::Idle && !bAllowWhenIdle)
			{
				outError =
					"Engine protocol command requires a completed initialization.";
				return false;
			}
			if (m_state != EState::Ready &&
				!(m_state == EState::Idle && bAllowWhenIdle))
			{
				outError =
					"Engine protocol command is unavailable during lifecycle transition.";
				return false;
			}

			++m_numActiveOperations;
			return true;
		}

		void ReleaseOperation()
		{
			{
				const std::lock_guard<std::mutex> lock(m_mutex);
				if (m_numActiveOperations > 0)
				{
					--m_numActiveOperations;
				}
			}
			m_condition.notify_all();
		}

		void Reset()
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_condition.wait(lock, [this]()
				{
					return m_numActiveOperations == 0 &&
						!m_bInitializationActive &&
						!m_bStartActive &&
						m_state != EState::Initializing &&
						m_state != EState::ShuttingDown;
				});
			m_state = EState::Idle;
			m_bStartIssued = false;
			m_bStartActive = false;
			m_bStopRequested = false;
		}

	private:
		enum class EState : uint8_t
		{
			Idle,
			Initializing,
			Ready,
			ShuttingDown,
			ShutdownComplete
		};

		std::mutex m_mutex{};
		std::condition_variable m_condition{};
		EState m_state = EState::Idle;
		uint32_t m_numActiveOperations = 0;
		bool m_bInitializationActive = false;
		bool m_bStartIssued = false;
		bool m_bStartActive = false;
		bool m_bStopRequested = false;
	};
}
