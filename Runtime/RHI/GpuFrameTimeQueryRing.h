#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Sailor::RHI
{
	template<uint32_t WindowSize>
	class TGpuTimingAverage final
	{
		static_assert(WindowSize > 0u);

	public:
		bool AddSample(float milliseconds)
		{
			if (!std::isfinite(milliseconds) || milliseconds < 0.0f)
			{
				return false;
			}

			if (m_sampleCount == WindowSize)
			{
				m_sum -= m_samples[m_nextSample];
			}
			else
			{
				++m_sampleCount;
			}

			m_samples[m_nextSample] = milliseconds;
			m_sum += milliseconds;
			m_nextSample = (m_nextSample + 1u) % WindowSize;
			return true;
		}

		float GetAverage() const
		{
			return m_sampleCount > 0u ?
				static_cast<float>(m_sum / static_cast<double>(m_sampleCount)) :
				0.0f;
		}

		uint32_t GetSampleCount() const
		{
			return m_sampleCount;
		}

	private:
		std::array<float, WindowSize> m_samples{};
		double m_sum = 0.0;
		uint32_t m_sampleCount = 0u;
		uint32_t m_nextSample = 0u;
	};

	inline bool TryResolveGpuFrameTimeMilliseconds(
		uint64_t beginTimestamp,
		uint64_t endTimestamp,
		uint32_t validBits,
		float timestampPeriodNs,
		float& outMilliseconds)
	{
		if (validBits == 0u || validBits > 64u ||
			!std::isfinite(timestampPeriodNs) || timestampPeriodNs <= 0.0f)
		{
			return false;
		}

		const uint64_t timestampMask = validBits == 64u ?
			(std::numeric_limits<uint64_t>::max)() :
			(1ull << validBits) - 1ull;
		const uint64_t elapsedTicks =
			(endTimestamp - beginTimestamp) & timestampMask;
		const double milliseconds = static_cast<double>(elapsedTicks) *
			static_cast<double>(timestampPeriodNs) / 1000000.0;

		// A valid GPU frame cannot reasonably span a minute. Besides filtering
		// corrupted values, this rejects a stale end timestamp interpreted as a
		// full-width unsigned wrap (which otherwise reaches the Stats overlay as
		// an enormous positive duration).
		if (!std::isfinite(milliseconds) || milliseconds > 60000.0)
		{
			return false;
		}

		outMilliseconds = static_cast<float>(milliseconds);
		return true;
	}

	inline uint32_t CalculateGpuFramesPerSecond(float frameTimeMilliseconds)
	{
		if (!std::isfinite(frameTimeMilliseconds) ||
			frameTimeMilliseconds <= 0.0f)
		{
			return 0u;
		}

		const double framesPerSecond = 1000.0 /
			static_cast<double>(frameTimeMilliseconds);
		if (framesPerSecond >=
			static_cast<double>((std::numeric_limits<uint32_t>::max)()))
		{
			return (std::numeric_limits<uint32_t>::max)();
		}

		const uint32_t rounded =
			static_cast<uint32_t>(framesPerSecond + 0.5);
		return rounded > 0u ? rounded : 1u;
	}

	enum class EGpuFrameTimeQuerySlotState : uint8_t
	{
		Available,
		Recording,
		Issued
	};

	template<uint32_t NumSlots>
	class TGpuFrameTimeQueryRing final
	{
		static_assert(NumSlots > 0u);

	public:
		static constexpr uint32_t InvalidSlot =
			(std::numeric_limits<uint32_t>::max)();

		uint32_t Acquire()
		{
			for (uint32_t offset = 0u; offset < NumSlots; ++offset)
			{
				const uint32_t slot = (m_nextSlot + offset) % NumSlots;
				if (m_states[slot] != EGpuFrameTimeQuerySlotState::Available)
				{
					continue;
				}

				m_states[slot] = EGpuFrameTimeQuerySlotState::Recording;
				m_nextSlot = (slot + 1u) % NumSlots;
				return slot;
			}

			return InvalidSlot;
		}

		bool MarkIssued(uint32_t slot)
		{
			if (slot >= NumSlots ||
				m_states[slot] != EGpuFrameTimeQuerySlotState::Recording)
			{
				return false;
			}

			m_states[slot] = EGpuFrameTimeQuerySlotState::Issued;
			return true;
		}

		bool MarkCompleted(uint32_t slot)
		{
			if (slot >= NumSlots ||
				m_states[slot] != EGpuFrameTimeQuerySlotState::Issued)
			{
				return false;
			}

			m_states[slot] = EGpuFrameTimeQuerySlotState::Available;
			return true;
		}

		bool CancelRecording(uint32_t slot)
		{
			if (slot >= NumSlots ||
				m_states[slot] != EGpuFrameTimeQuerySlotState::Recording)
			{
				return false;
			}

			m_states[slot] = EGpuFrameTimeQuerySlotState::Available;
			return true;
		}

		EGpuFrameTimeQuerySlotState GetState(uint32_t slot) const
		{
			return slot < NumSlots ?
				m_states[slot] :
				EGpuFrameTimeQuerySlotState::Available;
		}

	private:
		EGpuFrameTimeQuerySlotState m_states[NumSlots]{};
		uint32_t m_nextSlot = 0u;
	};
}
