#pragma once
#include "Memory.h"

namespace Sailor::Memory
{
	template<typename TPtrType = void*, typename TAllocator = HeapAllocator, uint32_t blockSize = 1024>
	class TMemoryBlockAllocator
	{
	public:

		class MemoryBlock;

		class TData
		{
		public:

			TData() = default;

			TData(size_t offset, size_t size, TPtrType ptr, TMemoryBlockAllocator* allocator, size_t blockIndex) :
				m_offset(offset),
				m_size(size),
				m_ptr(ptr),
				m_blockIndex(blockIndex),
				m_allocator(allocator)
			{}

			size_t m_offset{};
			size_t m_size{};
			TPtrType m_ptr{};

			void ShiftPointer(size_t offset)
			{
				m_offset += offset;
			}

			size_t Offset(const TData& from, const TData& to)
			{
				return (size_t)(to.m_offset - from.m_offset - from.m_size);
			}

			const TPtrType operator*() const
			{
				return Sailor::Memory::GetAddress<TData, TPtrType>(*this);
			}

			TPtrType operator*()
			{
				return Sailor::Memory::GetAddress<TData, TPtrType>(*this);
			}

			void Free()
			{
				m_allocator->Free(*this);
			}

		protected:

			void Clear()
			{
				m_offset = 0;
				m_size = 0;
				m_ptr = nullptr;
				m_blockIndex = 0;
				m_allocator = nullptr;
			}

			TMemoryBlockAllocator* m_allocator;
			size_t m_blockIndex;

			friend class TMemoryBlockAllocator;
			friend class MemoryBlock;

			friend void Sailor::Memory::Free<TData, TPtrType, TAllocator>(TData& ptr, TAllocator* allocator);
		};

		class MemoryBlock
		{
		public:

			MemoryBlock(size_t size) :
				m_size(size),
				m_freeSpace(size),
				m_freeSpaceLayout({ {0, size} }),
				m_index(InvalidIndex)
			{
				//std::cout << "Allocate Block " << (int32_t)size << std::endl;
				m_ptr = Sailor::Memory::Allocate<TData, TPtrType, TAllocator>(size, m_allocator);
			}

			~MemoryBlock()
			{
				//std::cout << "Free Block " << (int32_t)m_size << std::endl;
				if (m_index != InvalidIndex)
				{
					Sailor::Memory::Free<TData, TPtrType, TAllocator>(m_ptr, m_allocator);
				}
			}

			MemoryBlock(const MemoryBlock& memoryBlock) = delete;
			MemoryBlock& operator=(const MemoryBlock& memoryBlock) = delete;

			MemoryBlock(MemoryBlock&& memoryBlock)
			{
				m_ptr = memoryBlock.m_ptr;
				m_size = memoryBlock.m_size;
				m_freeSpace = memoryBlock.m_freeSpace;
				m_index = memoryBlock.m_index;
				m_owner = memoryBlock.m_owner;
				m_allocator = memoryBlock.m_allocator;
				m_freeSpaceLayout = std::move(memoryBlock.m_freeSpaceLayout);

				memoryBlock.m_allocator = nullptr;
				memoryBlock.m_owner = nullptr;
				memoryBlock.m_index = InvalidIndex;
				memoryBlock.m_freeSpace = 0;
				memoryBlock.m_size = 0;
			}

			TData Allocate(size_t size)
			{
				//std::cout << "Acquire " << (int32_t)size << std::endl;

				size_t offset = FindPlace(size);

				assert(offset != InvalidIndex);

				// todo: optimize from O(n) to O(logN)
				for (auto& freeSpace : m_freeSpaceLayout)
				{
					if (freeSpace.first == offset)
					{
						freeSpace.first += size;
						freeSpace.second -= size;
						break;
					}
				}

				m_freeSpace -= size;
				return TData(offset, size, m_ptr.m_ptr, m_owner, m_index);
			}

			void Free(TData& ptr)
			{
				//std::cout << "Release " << (int32_t)ptr.m_size << std::endl;

				if (!ptr.m_ptr)
				{
					return;
				}

				m_freeSpace += ptr.m_size;

				// todo: optimize from O(n) to O(logN)
				// place element in the front of list
				auto placeToPush = std::begin(m_freeSpaceLayout);
				for (auto it = std::begin(m_freeSpaceLayout); it != std::end(m_freeSpaceLayout); it++)
				{
					auto& freeSpace = *it;
					if (ptr.m_offset + ptr.m_size == freeSpace.first)
					{
						freeSpace.first = ptr.m_offset;
						freeSpace.second += ptr.m_size;

						//std::stable_sort(it, std::end(m_freeSpaceLayout), [](auto lhs, auto rhs) { return lhs.second < rhs.second; });

						ptr.Clear();
						return;
					}
					else if (ptr.m_size < freeSpace.second)
					{
						placeToPush = it;
					}
				}

				placeToPush++;
				m_freeSpaceLayout.insert(placeToPush, { ptr.m_offset, ptr.m_size });
				//std::stable_sort(placeToPush, std::end(m_freeSpaceLayout), [](auto lhs, auto rhs) { return lhs.second < rhs.second; });
				ptr.Clear();
			}

			size_t FindPlace(size_t size) const
			{
				if (size > m_freeSpace)
				{
					return InvalidIndex;
				}

				// optimize to O(logn)
				for (auto& offset : m_freeSpaceLayout)
				{
					if (offset.second >= size)
					{
						return offset.first;
					}
				}

				return InvalidIndex;
			}

			size_t GetTotalSize() const { return m_size; }

			bool IsEmpty() const { return m_freeSpace == m_size; }

		private:

			TData m_ptr;
			size_t m_size;
			size_t m_freeSpace;
			size_t m_index{ InvalidIndex };
			TMemoryBlockAllocator* m_owner;
			TAllocator* m_allocator;
			std::vector<std::pair<size_t, size_t>> m_freeSpaceLayout;

			friend class TMemoryBlockAllocator;
		};

		TMemoryBlockAllocator() = default;
		TMemoryBlockAllocator(const TMemoryBlockAllocator&) = delete;
		TMemoryBlockAllocator& operator= (const TMemoryBlockAllocator&) = delete;

		TData Allocate(size_t size)
		{
			auto& block = FindMemoryBlock(size);
			return block.Allocate(size);
		}

		void Free(TData& data)
		{
			if (data.m_ptr)
			{
				m_memoryBlocks[data.m_blockIndex].Free(data);
			}
		}

		virtual ~TMemoryBlockAllocator()
		{
			m_memoryBlocks.clear();
			return;

			for (auto& block : m_memoryBlocks)
			{
				//block->~MemoryBlock();
				//TBlockAllocator::Free(block, &m_blockAllocator);
			}
		}

		MemoryBlock& GetMemoryBlock(uint32_t index) const { return m_memoryBlocks[index]; }

	private:

		MemoryBlock& FindMemoryBlock(size_t size)
		{
			for (int32_t index = (int32_t)(m_memoryBlocks.size() - 1); index >= 0; index--)
			{
				auto& block = m_memoryBlocks[(uint32_t)index];
				if (block.FindPlace(size) != InvalidIndex)
				{
					return block;
				}
			}

			auto block = AllocateBlock(size);
			block.m_index = m_memoryBlocks.size();
			m_totalSpace += block.GetTotalSize();
			m_memoryBlocks.push_back(std::move(block));

			return m_memoryBlocks[m_memoryBlocks.size() - 1];
		}

		MemoryBlock AllocateBlock(size_t requestSize)
		{
			auto size = (size_t)std::max((uint32_t)requestSize, (uint32_t)blockSize);
			MemoryBlock block = MemoryBlock(size);

			block.m_owner = this;
			block.m_allocator = &m_allocator;

			return block;
		}

		bool FreeBlock(MemoryBlock* block) const
		{
			if (block->IsEmpty())
			{
				m_totalSpace -= block->m_size;
				//block->~MemoryBlock();
				//TBlockAllocator::Free(block, &m_blockAllocator);
				m_memoryBlocks.remove(*block);
				return true;
			}
			return false;
		}

		TAllocator m_allocator;

		size_t m_totalSpace = 0;
		std::vector<MemoryBlock> m_memoryBlocks;
	};
}