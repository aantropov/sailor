#pragma once
#include <cassert>
#include <functional>
#include <concepts>
#include <type_traits>
#include <iterator>
#include <algorithm>
#include "Core/Defines.h"

namespace Sailor
{
	template<typename T>
	concept IsTriviallyDestructible = std::is_trivially_destructible<T>::value;

	template<typename T>
	concept IsMoveConstructible = std::is_move_constructible<T>::value;

	template<typename T>
	concept IsCopyConstructible = std::is_copy_constructible<T>::value;

	template<typename T>
	using TPredicate = std::function<bool(const T&)>;
}