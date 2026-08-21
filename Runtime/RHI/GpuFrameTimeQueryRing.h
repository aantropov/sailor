#pragma once

#include <cstdint>
#include <limits>

namespace Sailor::RHI
{
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
