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
			size_t size = count * sizeof(TDataType);
			return Allocate(size, alignof(TDataType));
		}

		TMemoryPtr<TPtr> Allocate(size_t size, size_t alignment)
		{
			m_usedDataSpace += size;
			return GetOrCreateAllocator(size)->Allocate(size, alignment);
		}

		void Free(TMemoryPtr<TPtr>& data)
		{
			if (data.m_ptr)
			{
				m_usedDataSpace -= data.m_size;
				GetOrCreateAllocator(data.m_size)->Free(data);
			}
		}

		virtual ~TMultiPoolAllocator()
		{
			for (auto it : m_layout)
			{
				delete (it).second;
			}
			m_layout.clear();
		}

	private:

		TBlockAllocator<TGlobalAllocator, TPtr>* GetOrCreateAllocator(size_t size)
		{
			auto poolAllocator = m_layout.find(size);
			if (poolAllocator != m_layout.end())
			{
				return (*poolAllocator).second;
			}

			auto res = new TBlockAllocator<TGlobalAllocator, TPtr>(size * 32, size);
			m_layout[size] = res;
			return res;
		}

		std::unordered_map<size_t, TBlockAllocator<TGlobalAllocator, TPtr>*> m_layout;
		size_t m_usedDataSpace = 0;
	};
}