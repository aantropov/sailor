#pragma once
#include <memory>
#include <vector>

#include "Memory/UniquePtr.hpp"
#define InvalidIndexUINT64 UINT64_MAX

namespace Sailor::Memory
{
	namespace Internal
	{
		class PoolAllocator
		{
		public:

			PoolAllocator(size_t startPageSize = 2048) : m_pageSize(startPageSize) {}

			struct Header
			{
				size_t m_next = InvalidIndexUINT64;
				size_t m_nextFree = InvalidIndexUINT64;
				size_t m_prev = InvalidIndexUINT64;
				size_t m_prevFree = InvalidIndexUINT64;
				size_t m_pageIndex = 0;
				size_t m_size = 0;
				bool m_bIsFree : 1;
				uint8_t m_meta;
			};

			class Page
			{
			public:

				size_t m_totalSize = InvalidIndexUINT64;
				size_t m_occupiedSpace = InvalidIndexUINT64;
				void* m_pData = nullptr;
				size_t m_firstFree = InvalidIndexUINT64;
				size_t m_first = InvalidIndexUINT64;
				bool m_bIsInFreeList = true;

				bool IsEmpty() const { return m_occupiedSpace == sizeof(Header); }
				inline Header* MoveHeader(Header* block, int64_t shift);

				void* Allocate(size_t size, size_t alignment);
				void Free(void* pData);

				void Clear();

				size_t GetMinAllowedEmptySpace() const;
			};

			void* Allocate(size_t size, size_t alignment);
			void Free(void* ptr);

			size_t GetOccupiedSpace() const
			{
				return 0;
			}
			~PoolAllocator();

		private:

			bool RequestPage(Page& page, size_t size, size_t pageIndex) const;

			const size_t m_pageSize = 2048;

			std::vector<Page> m_pages;
			std::vector<size_t> m_freeList;
			std::vector<size_t> m_emptyPages;
		};

		class SmallPoolAllocator
		{
		public:

			struct SmallHeader
			{
				uint16_t m_pageIndex = 0;
				uint16_t m_id = 0;
				uint8_t m_size = 0;
				uint8_t m_meta = 0;
			};

			class SmallPage
			{
			public:

				static constexpr  uint32_t m_size = 65536;

				SmallPage() = default;
				SmallPage(uint8_t blockSize, uint16_t pageIndex);

				uint16_t m_numAllocs = 0;
				uint16_t m_pageIndex = 0;
				void* m_pData = nullptr;
				uint8_t m_blockSize = 0;
				bool m_bIsInFreeList = true;
				std::vector<uint16_t> m_freeList;

				void* Allocate();
				void Free(void* ptr);
				void Clear();

				size_t GetMaxBlocksNum() const;
				uint16_t GetOccupiedSpace() const;

				bool IsFull() const;
				bool IsEmpty() const;
			};

			SmallPoolAllocator(uint8_t blockSize) : m_blockSize(blockSize) {}
			~SmallPoolAllocator();

			bool RequestPage(SmallPage& page, uint8_t blockSize, uint16_t pageIndex) const;

			void* Allocate();
			void Free(void* ptr);

		private:

			uint8_t m_blockSize = 0;
			std::vector<SmallPage> m_pages;
			std::vector<uint16_t> m_freeList;
			std::vector<uint16_t> m_emptyPages;
		};
	}
	
	class HeapAllocator
	{
	public:

		HeapAllocator();
		void* Allocate(size_t size, size_t alignment = 8);
		void* Reallocate(void* ptr, size_t size, size_t alignment = 8);
		void Free(void* ptr);

	private:

		inline size_t CalculateAlignedSize(size_t blockSize) const;
		std::vector<TUniquePtr<Internal::SmallPoolAllocator>> m_smallAllocators;
		Internal::PoolAllocator m_allocator;
	};
}