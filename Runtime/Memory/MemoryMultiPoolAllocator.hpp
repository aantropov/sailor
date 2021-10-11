#pragma once
#include "Memory.h"
#include <algorithm>
#include <unordered_map>

namespace Sailor::Memory
{
	template<typename TGlobalAllocator, typename TPtr>
	class TMultiPoolAllocator
	{
	public:

		TMultiPoolAllocator() = default;
		TMultiPoolAllocator(const TMultiPoolAllocator&) = delete;
		TMultiPoolAllocator& operator= (const TMultiPoolAllocator&) = delete;

		template<typename TDataType>
		TMemoryPtr<TPtr> Allocate(uint32_t count)
		{
			assert(sizeof(TDataType) == elementSize);

			size_t size = count * sizeof(TDataType);
			return Allocate(size, alignof(TDataType));
		}

		TMemoryPtr<TPtr> Allocate(size_t size, size_t alignment)
		{
			return GetOrCreateAllocator(size)->Allocate(size, alignment);
		}

		void Free(TMemoryPtr<TPtr>& data)
		{
			if (data.m_ptr)
			{
				GetOrCreateAllocator(data.m_size)->Free(data);
			}
		}

		virtual ~TMultiPoolAllocator()
		{
			m_layout.clear();
		}

	private:

		TPoolAllocator<TGlobalAllocator, TPtr> allocator{ 4, 4 };
		TSharedPtr<TPoolAllocator<TGlobalAllocator, TPtr>> GetOrCreateAllocator(size_t size)
		{
			auto poolAllocator = m_layout.find(size);
			if (poolAllocator != m_layout.end())
			{
				return (*poolAllocator).second;
			}

			auto res = TSharedPtr<TPoolAllocator<TGlobalAllocator, TPtr>>::Make(4, size);
			m_layout[size] = res;
			return res;
		}

		std::unordered_map<size_t, TSharedPtr<TPoolAllocator<TGlobalAllocator, TPtr>>> m_layout;
		size_t m_usedDataSpace = 0;
	};
}