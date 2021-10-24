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

				const uint32_t bestFit = 4;
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
			MemoryBlock& operator=(MemoryBlock&& memoryBlock) = default;

			MemoryBlock(MemoryBlock&& memoryBlock) noexcept
			{
				m_ptr = memoryBlock.m_ptr;
				m_blockSize = memoryBlock.m_blockSize;
				m_emptySpace = memoryBlock.m_emptySpace;
				m_blockIndex = memoryBlock.m_blockIndex;
				m_owner = memoryBlock.m_owner;
				m_layout = std::move(memoryBlock.m_layout);
				m_notTrackedEmptySpace = memoryBlock.m_notTrackedEmptySpace;
				m_bIsOutOfSync = memoryBlock.m_bIsOutOfSync;

				memoryBlock.m_owner = nullptr;
				memoryBlock.m_blockIndex = InvalidIndex;
				memoryBlock.m_emptySpace = 0;
				memoryBlock.m_blockSize = 0;
				memoryBlock.m_layout.clear();
				memoryBlock.m_notTrackedEmptySpace = 0;
				memoryBlock.m_bIsOutOfSync = false;
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
					m_layout.pop_back();
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

				if (!m_bIsOutOfSync)
				{
					auto lower = std::lower_bound(m_layout.begin(), m_layout.end(), std::pair{ ptr.m_offset, ptr.m_size + ptr.m_alignmentOffset }, [](auto& lhs, auto& rhs) { return lhs.first < rhs.first; });
					if (lower != m_layout.end())
					{
						const size_t i = lower - m_layout.begin();
						bool bMerged = false;

						const bool leftMerged = i > 0 && m_layout[i - 1].first + m_layout[i - 1].second == ptr.m_offset;
						const bool rightMerged = ptr.m_offset + ptr.m_size + ptr.m_alignmentOffset == m_layout[i].first;

						if (leftMerged && rightMerged)
						{
							m_layout[i - 1].second += ptr.m_size + ptr.m_alignmentOffset + m_layout[i].second;
							m_layout.erase(m_layout.begin() + i);

							ptr.Clear();
							return;
						}

						if (leftMerged)
						{
							m_layout[i - 1].second += ptr.m_size + ptr.m_alignmentOffset;
							ptr.Clear();
							return;
						}
						else if (rightMerged)
						{
							m_layout[i].first = ptr.m_offset;
							m_layout[i].second += ptr.m_size + ptr.m_alignmentOffset;
							ptr.Clear();
							return;
						}
					}

					if (m_layout.size() > 0)
					{
						if (const bool leftMerged = (((m_layout[m_layout.size() - 1]).first + (m_layout[m_layout.size() - 1]).second) == ptr.m_offset))
						{
							(m_layout[m_layout.size() - 1]).second += ptr.m_size + ptr.m_alignmentOffset;
							ptr.Clear();
							return;
						}
					}
					
					m_layout.insert(lower, { ptr.m_offset, ptr.m_size + ptr.m_alignmentOffset });
				}
				else
				{
					m_notTrackedEmptySpace += ptr.m_size + ptr.m_alignmentOffset;
				}

				ptr.Clear();
			}

			bool FindLocationInLayout(size_t size, size_t alignment, uint32_t& layoutIndex, uint32_t& alignmentOffset)
			{
				if (size > m_emptySpace - m_notTrackedEmptySpace)
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

			void Clear()
			{
				Sailor::Memory::Free<TMemoryPtr<TPtr>, TPtr, TGlobalAllocator>(m_ptr, &m_owner->m_dataAllocator);
				m_blockSize = 0;
				m_emptySpace = 0;
				m_layout.clear();
				m_notTrackedEmptySpace = 0;
				m_blockIndex = InvalidIndex;
				m_bIsOutOfSync = false;
			}

		private:

			TMemoryPtr<TPtr> m_ptr;
			size_t m_blockSize;
			size_t m_emptySpace;
			uint32_t m_blockIndex{ InvalidIndex };
			bool m_bIsOutOfSync = false;
			size_t m_notTrackedEmptySpace = 0;
			TPoolAllocator* m_owner;

			std::vector<std::pair<size_t, size_t>> m_layout;

			friend class TPoolAllocator;
		};

		TPoolAllocator(size_t startBlockSize = 128, size_t elementSize = 32) : m_startBlockSize(startBlockSize), m_elementSize(elementSize) {}
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
			m_mutex.lock();
			//assert(size % m_elementSize == 0);

			uint32_t layoutIndex;
			uint32_t blockLayoutIndex;
			uint32_t alignmentOffset;
			FindMemoryBlock(size, alignment, layoutIndex, blockLayoutIndex, alignmentOffset);

			const uint32_t blockIndex = m_layout[layoutIndex];
			auto& block = m_blocks[blockIndex];
			auto res = block.Allocate(blockLayoutIndex, size, alignmentOffset);

			if (HeuristicToSkipBlocks(block.GetOccupation(), block.m_blockSize))
			{
				std::iter_swap(m_layout.begin() + layoutIndex, m_layout.end() - 1);
				m_layout.pop_back();
			}
			m_mutex.unlock();
			return res;
		}

		void Free(TMemoryPtr<TPtr>& data)
		{
			m_mutex.lock();
			if (data.m_ptr)
			{
				const uint32_t index = data.m_blockIndex;
				const float prevOccupation = m_blocks[index].GetOccupation();
				m_blocks[index].Free(data);
				const float currentOccupation = m_blocks[index].GetOccupation();

				if (HeuristicToSkipBlocks(prevOccupation, m_blocks[index].m_blockSize) &&
					!HeuristicToSkipBlocks(currentOccupation, m_blocks[index].m_blockSize) &&
					!m_blocks[index].m_bIsOutOfSync)
				{
					m_layout.push_back(index);
				}

				if (!m_blocks[index].m_bIsOutOfSync && HeuristicToMarkBlockDead(m_blocks[index].m_blockSize, m_blocks[index].m_layout.size()))
				{
					m_blocks[index].m_bIsOutOfSync = true;
				}

				if (m_blocks[index].m_bIsOutOfSync && m_blocks[index].IsEmpty())
				{
					m_blocks[index].m_bIsOutOfSync = false;
					m_blocks[index].m_layout.clear();
					m_blocks[index].m_notTrackedEmptySpace = 0;

					if (std::find(m_layout.begin(), m_layout.end(), index) == m_layout.end())
					{
						m_layout.push_back(index);
					}
				}

				if (HeuristicToFreeBlock(m_blocks[index].m_blockSize, m_layout.size()))
				{
					TryFreeBlock(m_blocks[index]);
				}
			}
			m_mutex.unlock();
		}

		virtual ~TPoolAllocator()
		{
			m_blocks.clear();
			m_layout.clear();
		}

		MemoryBlock& GetMemoryBlock(uint32_t index) const { return m_blocks[index]; }
		size_t GetOccupiedSpace() const { return m_usedDataSpace; }

		TGlobalAllocator& GetGlobalAllocator() { return m_dataAllocator; }

	private:

		std::mutex m_mutex;
		
		static constexpr uint32_t InvalidIndex = (uint32_t)-1;

		bool HeuristicToSkipBlocks(float occupation, size_t blockSize) const
		{
			// We assume that block is fully occupied if there is less space than the space of 1 element
			const float border = 1.0f - (float)m_elementSize / blockSize;
			return occupation > border;
		}

		bool HeuristicToMarkBlockDead(size_t blockSize, size_t segmentation) const
		{
			// Directly affects memory overhead & deallocation cost
			const uint32_t highSegmentation = 6000;
			return segmentation > highSegmentation;
		}

		bool HeuristicToFreeBlock(size_t blockSize, size_t countFreeBlocks) const
		{
			// Free small blocks
			return countFreeBlocks > 2 || (countFreeBlocks > 1 && (blockSize / (float)m_usedDataSpace) < 0.25f);
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

		bool TryFreeBlock(MemoryBlock& block)
		{
			if (block.IsEmpty())
			{
				m_usedDataSpace -= block.m_blockSize;
				m_layout.erase(std::remove(m_layout.begin(), m_layout.end(), block.m_blockIndex), m_layout.end());

				block.Clear();

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