#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include <iterator>
#include <algorithm>
#include "Core/Defines.h"
#include "Math/Math.h"
#include "Containers/Concepts.h"

namespace Sailor
{
	template<typename TElementType, typename TAllocator = Memory::DefaultGlobalAllocator>
	class SAILOR_API TOctree final
	{
	public:

		// Constructors & Destructor
		TOctree() {}
		~TOctree() { Clear(); }

		void Clear() {}

	protected:

		TAllocator m_allocator{};
	};
}