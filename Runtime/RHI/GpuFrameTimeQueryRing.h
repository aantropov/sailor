#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace Sailor::RHI
{
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
