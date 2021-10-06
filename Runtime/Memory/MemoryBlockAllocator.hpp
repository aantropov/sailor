#pragma once
#include "Memory.h"

namespace Sailor::Memory
{
	template<typename TPtrType = void*, typename TBlockAllocator = HeapAllocator, uint32_t blockSize = 1024>
	class TMemoryBlockAllocator
	{
	public:

		class MemoryBlock;

		class TData
		{
		public:

			TData() = default;

			TData(size_t offset, size_t size, TPtrType ptr, MemoryBlock* block) :
				m_offset(offset),
				m_size(size),
				m_ptr(ptr),
				m_block(block)
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
				if (m_block != nullptr)
				{
					m_block->Free(*this);
				}
			}

		protected:

			void Clear()
			{
				m_offset = 0;
				m_size = 0;
				m_ptr = nullptr;
				m_block = nullptr;
			}

			MemoryBlock* m_block{};

			friend class TMemoryBlockAllocator;
			friend class MemoryBlock;

			friend void Sailor::Memory::Free<TData, TPtrType>(TData& ptr);
		};

		class MemoryBlock
		{
		public:

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
				return TData(offset, size, m_ptr.m_ptr, this);
			}

			void Free(TData& ptr)
			{
				//std::cout << "Release " << (int32_t)ptr.m_size << std::endl;

				m_freeSpace += ptr.m_size;

				// todo: optimize from O(n) to O(logN)
				for (auto& freeSpace : m_freeSpaceLayout)
				{
					if (ptr.m_offset + ptr.m_size == freeSpace.first)
					{
						freeSpace.first = ptr.m_offset;
						freeSpace.second += ptr.m_size;
						ptr.Clear();
						break;
					}
				}

				m_freeSpaceLayout.push_back({ ptr.m_offset, ptr.m_size });
				ptr.Clear();
			}

			size_t FindPlace(size_t size) const
			{
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

			bool IsEmpty() const { return m_freeSpace == m_size; }

			MemoryBlock(size_t size) :
				m_size(size),
				m_freeSpace(size),
				m_freeSpaceLayout({ {0, size} })
			{
				//std::cout << "Allocate Block " << (int32_t)size << std::endl;
				m_ptr = Sailor::Memory::Allocate<TData, TPtrType>(size);
			}

			~MemoryBlock()
			{
				//std::cout << "Free Block " << (int32_t)m_size << std::endl;
				Sailor::Memory::Free<TData, TPtrType>(m_ptr);
			}

		private:

			TData m_ptr;
			size_t m_size;
			size_t m_freeSpace;

			std::list<std::pair<size_t, size_t>> m_freeSpaceLayout;
		};

		TData Allocate(size_t size)
		{
			auto block = FindMemoryBlock(size);
			return block->Allocate(size);
		}

		void Free(TData& data)
		{
			data.m_block->Free(data);
		}

		virtual ~TMemoryBlockAllocator()
		{
			for (auto block : m_memoryBlocks)
			{
				block->~MemoryBlock();
				TBlockAllocator::Free(block);
			}
		}

	private:

		MemoryBlock* FindMemoryBlock(size_t size)
		{
			for (auto block : m_memoryBlocks)
			{
				if (block->FindPlace(size) != InvalidIndex)
				{
					return block;
				}
			}

			auto block = AllocateBlock(size);
			m_memoryBlocks.push_back(block);

			m_totalSpace += size;

			return block;
		}

		MemoryBlock* AllocateBlock(size_t requestSize) const
		{
			auto size = (size_t)std::max((uint32_t)requestSize, (uint32_t)blockSize);
			auto block = static_cast<MemoryBlock*>(TBlockAllocator::Allocate(sizeof(MemoryBlock)));
			new (block) MemoryBlock(size);

			return block;
		}

		bool FreeBlock(MemoryBlock* block) const
		{
			if (block->IsEmpty())
			{
				m_totalSpace -= block->m_size;
				block->~MemoryBlock();
				TBlockAllocator::Free(block);
				return true;
			}
			return false;
		}

		size_t m_totalSpace = 0;
		std::list<MemoryBlock*> m_memoryBlocks;
	};
}