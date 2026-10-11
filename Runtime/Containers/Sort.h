#pragma once

#include "Containers/Vector.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace Sailor
{
	// Cancellation may leave moved-from elements; use only on disposable working data.
	template<typename TIterator, typename TCompare, typename TContinue>
	bool CancellableStableSort(TIterator first, TIterator last, const TCompare& compare, const TContinue& shouldContinue)
	{
		constexpr size_t BatchSize = 1024u;
		if (first == last) return shouldContinue();
		const size_t count = static_cast<size_t>(last - first);
		for (size_t begin = 0u; begin < count; begin += BatchSize)
		{
			if (!shouldContinue()) return false;
			std::stable_sort(first + begin, first + (std::min)(begin + BatchSize, count), compare);
		}
		if (count <= BatchSize) return shouldContinue();

		if (!shouldContinue()) return false;
		TVector<typename std::iterator_traits<TIterator>::value_type> scratch(count);
		for (size_t width = BatchSize; width < count; width *= 2u)
		{
			for (size_t begin = 0u; begin < count; begin += width * 2u)
			{
				const size_t middle = (std::min)(begin + width, count);
				const size_t end = (std::min)(begin + width * 2u, count);
				size_t left = begin, right = middle;
				for (size_t output = begin; output < end; ++output)
				{
					if (output % BatchSize == 0u && !shouldContinue()) return false;
					const bool takeLeft = left < middle && (right == end || !compare(first[right], first[left]));
					scratch[output] = std::move(first[takeLeft ? left++ : right++]);
				}
			}
			for (size_t index = 0u; index < count; ++index)
			{
				if (index % BatchSize == 0u && !shouldContinue()) return false;
				first[index] = std::move(scratch[index]);
			}
		}
		return shouldContinue();
	}
}
