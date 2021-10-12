#pragma once
#include "Memory.h"
#include <algorithm>

namespace Sailor::Memory
{
	class TPoolAllocatorBase {};

	template<typename TGlobalAllocator, typename TPtr>
	class TPoolAllocator : public TPoolAllocatorBase
	{
	public:

		class MemoryBlock
		{
		public:

			MemoryBlock(size_t size, TPoolAllocator* owner) :
				m_blockSize(size),
				m_emptySpace(size),
				m_layout({ {0, size} }),
				m_blockIndex(InvalidIndex),
				m_owner(owner)
			{
				m_ptr = Sailor::Memory::Allocate<TMemoryPtr<TPtr>, TPtr, TGlobalAllocator>(size, &m_owner->m_dataAllocator);

				const uint32_t bestFit = 32;
				m_layout.reserve(bestFit);
			}

			~MemoryBlock()
			{
				if (m_blockIndex != InvalidIndex)
				{
					Sailor::Memory::Free<TMemoryPtr<TPtr>, TPtr, TGlobalAllocator>(m_ptr, &m_owner->m_dataAllocator);
				}

				m_layout.clear();
			}

			MemoryBlock(const MemoryBlock& memoryBlock) = delete;
			MemoryBlock& operator=(const MemoryBlock& memoryBlock) = delete;

			MemoryBlock(MemoryBlock&& memoryBlock) noexcept
			{
				m_ptr = memoryBlock.m_ptr;
				m_blockSize = memoryBlock.m_blockSize;
				m_emptySpace = memoryBlock.m_emptySpace;
				m_blockIndex = memoryBlock.m_blockIndex;
				m_owner = memoryBlock.m_owner;
				m_layout = std::move(memoryBlock.m_layout);

				memoryBlock.m_owner = nullptr;
				memoryBlock.m_blockIndex = InvalidIndex;
				memoryBlock.m_emptySpace = 0;
				memoryBlock.m_blockSize = 0;
			}

			TMemoryPtr<TPtr> Allocate(uint32_t layoutIndex, size_t size, uint32_t alignmentOffset)
			{
				assert(layoutIndex != InvalidIndex);

				auto& emptySpace = m_layout[layoutIndex];

				size_t offset = emptySpace.first;
				emptySpace.first += size + alignmentOffset;
				emptySpace.second -= (size + alignmentOffset);

				if (emptySpace.second == 0)
				{
					for (int32_t i = layoutIndex; i < m_layout.size() - 1; i++)
					{
						m_layout[i] = m_layout[i + 1];
					}
				}

				m_emptySpace -= (size + alignmentOffset);
				return TMemoryPtr<TPtr>(offset, alignmentOffset, size, m_ptr.m_ptr, m_blockIndex);
			}

			void Free(TMemoryPtr<TPtr>& ptr)
			{
				if (!ptr.m_ptr)
				{
					return;
				}

				m_emptySpace += ptr.m_size + ptr.m_alignmentOffset;

				if (m_layout.size() > 0 && ptr.m_offset > m_layout[m_layout.size() - 1].first)
				{
					m_layout.push_back({ ptr.m_offset, ptr.m_size + ptr.m_alignmentOffset });
					ptr.Clear();
					return;
				}

				if (m_layout.size() > 0 && ptr.m_offset < m_layout[0].first)
				{
					m_layout.push_back({ ptr.m_offset, ptr.m_size + ptr.m_alignmentOffset });
					std::iter_swap(m_layout.begin(), m_layout.end() - 1);
					ptr.Clear();
					return;
				}

				auto lower = std::lower_bound(m_layout.begin(), m_layout.end(), std::pair{ ptr.m_offset, ptr.m_size + ptr.m_alignmentOffset }, [](auto& lhs, auto& rhs) { return lhs.first < rhs.first; });

				if (lower != m_layout.end())
				{
					const size_t i = lower - m_layout.begin();
					if (i > 0 && m_layout[i - 1].first + m_layout[i - 1].second == ptr.m_offset)
					{
						m_layout[i - 1].second += ptr.m_size + ptr.m_alignmentOffset;
						ptr.Clear();
						return;
					}
					else if (ptr.m_offset + ptr.m_size + ptr.m_alignmentOffset == m_layout[i].first)
					{
						m_layout[i].first = ptr.m_offset;
						ptr.Clear();
						return;
					}
				}

				m_layout.insert(lower, { ptr.m_offset, ptr.m_size + ptr.m_alignmentOffset });
				ptr.Clear();
			}

			bool FindLocationInLayout(size_t size, size_t alignment, uint32_t& layoutIndex, uint32_t& alignmentOffset)
			{
				if (size > m_emptySpace)
				{
					return false;
				}

				for (uint32_t i = 0; i != (uint32_t)m_layout.size(); i++)
				{
					if (Align(size, alignment, Memory::Shift(*m_ptr, m_layout[i].first), m_layout[i].second, alignmentOffset))
					{
						layoutIndex = i;
						return true;
					}
				}

				return false;
			}

			size_t GetBlockSize() const { return m_blockSize; }
			float GetOccupation() const { return 1.0f - (float)m_emptySpace / m_blockSize; }

			bool IsFull() const { return m_emptySpace == 0; }
			bool IsEmpty() const { return m_emptySpace == m_blockSize; }

		private:

			TMemoryPtr<TPtr> m_ptr;
			size_t m_blockSize;
			size_t m_emptySpace;
			uint32_t m_blockIndex{ InvalidIndex };
			TPoolAllocator* m_owner;

			std::vector<std::pair<size_t, size_t>> m_layout;

			friend class TPoolAllocator;
		};

		TPoolAllocator(size_t startBlockSize = 4, size_t elementSize = 1) : m_startBlockSize(startBlockSize), m_elementSize(elementSize) {}
		TPoolAllocator(const TPoolAllocator&) = delete;
		TPoolAllocator(TPoolAllocator&&) = default;
		TPoolAllocator& operator= (const TPoolAllocator&) = delete;
		TPoolAllocator& operator= (TPoolAllocator&&) = default;

		template<typename TDataType>
		TMemoryPtr<TPtr> Allocate(uint32_t count)
		{
			//assert(sizeof(TDataType) == m_elementSize);

			size_t size = count * sizeof(TDataType);
			return Allocate(size, alignof(TDataType));
		}

		TMemoryPtr<TPtr> Allocate(size_t size, size_t alignment)
		{
			//assert(size % m_elementSize == 0);

			uint32_t layoutIndex;
			uint32_t blockLayoutIndex;
			uint32_t alignmentOffset;
			FindMemoryBlock(size, alignment, layoutIndex, blockLayoutIndex, alignmentOffset);

			const auto blockIndex = m_layout[layoutIndex];
			auto& block = m_blocks[blockIndex];
			auto res = block.Allocate(blockLayoutIndex, size, alignmentOffset);

			if (block.IsEmpty())
			{
				std::iter_swap(m_layout.begin() + layoutIndex, m_layout.end() - 1);
				m_layout.pop_back();
			}
			return res;
		}

		void Free(TMemoryPtr<TPtr>& data)
		{
			if (data.m_ptr)
			{
				const uint32_t index = data.m_blockIndex;
				const float prevOccupation = m_blocks[index].GetOccupation();
				m_blocks[index].Free(data);
				const float currentOccupation = m_blocks[index].GetOccupation();

				if (HeuristicToSkipBlocks(prevOccupation, m_blocks[index].m_blockSize) && !HeuristicToSkipBlocks(currentOccupation, m_blocks[index].m_blockSize))
				{
					m_layout.push_back(index);
				}
			}
		}

		virtual ~TPoolAllocator()
		{
			m_blocks.clear();
			m_layout.clear();
		}

		MemoryBlock& GetMemoryBlock(uint32_t index) const { return m_blocks[index]; }

	private:

		static constexpr uint32_t InvalidIndex = (uint32_t)-1;

		bool HeuristicToSkipBlocks(float occupation, size_t blockSize) const
		{
			const float border = 1.0f - (float)m_elementSize / blockSize;
			return occupation > border;
		}

		void FindMemoryBlock(size_t size, size_t alignment, uint32_t& outLayoutIndex, uint32_t& outBlockLayoutIndex, uint32_t& outAlignedOffset)
		{
			for (int32_t index = (int32_t)(m_layout.size() - 1); index >= 0; index--)
			{
				auto& block = m_blocks[m_layout[index]];
				if (block.FindLocationInLayout(size, alignment, outBlockLayoutIndex, outAlignedOffset))
				{
					outLayoutIndex = index;
					return;
				}
			}

			// Create new block
			MemoryBlock block = MemoryBlock((size_t)max((uint32_t)size, (uint32_t)(m_startBlockSize * pow(2, m_blocks.size()))), this);
			block.m_blockIndex = (uint32_t)m_blocks.size();
			outLayoutIndex = (uint32_t)m_layout.size();
			block.FindLocationInLayout(size, alignment, outBlockLayoutIndex, outAlignedOffset);
			m_layout.push_back(block.m_blockIndex);
			m_usedDataSpace += block.GetBlockSize();
			m_blocks.push_back(std::move(block));
		}

		bool FreeBlock(MemoryBlock& block) const
		{
			if (block.IsEmpty())
			{
				m_usedDataSpace -= block.m_size;
				m_blocks.remove(block);
				return true;
			}
			return false;
		}

		TGlobalAllocator m_dataAllocator;

		size_t m_startBlockSize;
		size_t m_elementSize = 1;
		size_t m_usedDataSpace = 0;
		std::vector<MemoryBlock> m_blocks;
		std::vector<uint32_t> m_layout;
	};
}