#pragma once
#include "Core/Defines.h"
#include "Containers/Concepts.h"
#include "Containers/Pair.h"
#include "Containers/List.h"
#include "Containers/Set.h"
#include "Containers/Map.h"
#include "Containers/ConcurrentSet.h"
#include "Containers/ConcurrentMap.h"
#include "Containers/Vector.h"
#include "Memory/LockFreeHeapAllocator.h"

namespace Sailor
{
#ifdef SAILOR_CONTAINERS_USE_LOCK_FREE_HEAP_ALLOCATOR
	template<typename TData, typename TAllocator = Memory::LockFreeHeapAllocator>	class TVector;
	template<typename TData, typename TAllocator = Memory::LockFreeHeapAllocator>	class TList;
	template<typename TData, typename TAllocator = Memory::LockFreeHeapAllocator>	class TSet;
	template<typename TKeyType, typename TValueType, typename TAllocator = Memory::LockFreeHeapAllocator> class TMap;
	template<typename TData, const uint32_t concurrencyLevel = 8, typename TAllocator = Memory::LockFreeHeapAllocator>	class TConcurrentSet;
	template<typename TKeyType, typename TValueType, const uint32_t concurrencyLevel = 8, typename TAllocator = Memory::LockFreeHeapAllocator>	class TConcurrentMap;
#else
	template<typename TData, typename TAllocator = Memory::MallocAllocator>	class TVector;
	template<typename TData, typename TAllocator = Memory::MallocAllocator>	class TList;
	template<typename TData, typename TAllocator = Memory::MallocAllocator>	class TSet;
	template<typename TKeyType, typename TValueType, typename TAllocator = Memory::MallocAllocator> class TMap;
	template<typename TData, const uint32_t concurrencyLevel = 8, typename TAllocator = Memory::MallocAllocator>	class TConcurrentSet;
	template<typename TKeyType, typename TValueType, const uint32_t concurrencyLevel = 8, typename TAllocator = Memory::MallocAllocator> class TConcurrentMap;
#endif
}