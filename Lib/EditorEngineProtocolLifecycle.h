#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace Sailor::Protocol
{
	class TEditorEngineProtocolLifecycleGate final
	{
	public:
		using FStartRoutine = void (*)(void* context);

		~TEditorEngineProtocolLifecycleGate()
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			if (!m_startThread.joinable())
			{
				return;
			}
			m_condition.wait(lock, [this]()
				{
					return !m_bStartActive;
				});
			m_startThread.join();
		}

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
			return TryBeginStartLocked(outError);
		}

		bool TryBeginStartAsync(
			void* context,
			FStartRoutine startRoutine,
			std::string& outError)
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			if (!startRoutine)
			{
				outError = "Engine start routine is unavailable.";
				return false;
			}
			if (!TryBeginStartLocked(outError))
			{
				return false;
			}
			if (m_startThread.joinable())
			{
				m_bStartIssued = false;
				m_bStartActive = false;
				outError = "The previous Engine start worker was not joined.";
				return false;
			}

			try
			{
				m_startThread = std::thread(
					[this, context, startRoutine]()
					{
						try
						{
							startRoutine(context);
						}
						catch (...)
						{
							// Async lifecycle work cannot propagate through the
							// request that already acknowledged its admission.
						}
						CompleteStart();
					});
			}
			catch (...)
			{
				m_bStartIssued = false;
				m_bStartActive = false;
				outError = "Failed to create the Engine start worker.";
				lock.unlock();
				m_condition.notify_all();
				return false;
			}
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

		bool IsStartActive()
		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			return m_bStartActive;
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

		void WaitForStartDrainAndJoin()
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_condition.wait(lock, [this]()
				{
					return !m_bStartActive;
				});
			if (m_startThread.joinable())
			{
				m_startThread.join();
			}
		}

		void CompleteShutdown()
		{
			{
				const std::lock_guard<std::mutex> lock(m_mutex);
				m_state = EState::ShutdownComplete;
				m_bInitializationActive = false;
				m_bStartIssued = false;
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
			if (m_startThread.joinable())
			{
				m_startThread.join();
			}
			m_state = EState::Idle;
			m_bStartIssued = false;
			m_bStartActive = false;
			m_bStopRequested = false;
		}

	private:
		bool TryBeginStartLocked(std::string& outError)
		{
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
		std::thread m_startThread{};
		EState m_state = EState::Idle;
		uint32_t m_numActiveOperations = 0;
		bool m_bInitializationActive = false;
		bool m_bStartIssued = false;
		bool m_bStartActive = false;
		bool m_bStopRequested = false;
	};
}
