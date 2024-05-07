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
	concept IsDefaultConstructible = std::is_default_constructible<T>::value;

	template<typename T>
	concept IsTriviallyDestructible = std::is_trivially_destructible<T>::value;

	template<typename T>
	concept IsMoveConstructible = std::is_move_constructible<T>::value;

	template<typename T>
	concept IsMoveAssignable = std::is_move_assignable<T>::value;

	template<typename T>
	concept IsCopyConstructible = std::is_copy_constructible<T>::value;

	template<typename T>
	concept IsCopyAssignable = std::is_copy_assignable<T>::value;

	template<typename T>
	concept IsTriviallyCopyable = std::is_trivially_copyable<T>::value;

	template<typename TBase, typename TDerived>
	concept IsBaseOf = std::is_base_of<TBase, TDerived>::value;

	template<typename T>
	concept IsEnum = std::is_enum<T>::value;

	template<typename T, typename R>
	concept IsSame = std::is_same<T, R>::value;

	template<typename T>
	concept NotVoid = !std::is_same_v<T, void>;

	template<typename T>
	concept TIsFunction = std::is_function<T>::value;

	template <typename T, typename U>
	concept TEqualityComparableWith = std::equality_comparable_with<T, U>;

	template<typename T>
	using TPredicate = std::function<bool(const T&)>;

	template<typename T>
	using TCompare = std::function<bool(const T&, const T&)>;

	struct EmptyType {};

	template<typename R, typename A>
	struct TFunction
	{
		using type = std::function<R(A)>;
	};

	template <typename R>
	struct TFunction<R, void>
	{
		using type = std::function<R()>;
	};

	template <typename A>
	struct TFunction<void, A>
	{
		using type = std::function<void(A)>;
	};

	template <>
	struct TFunction<void, void>
	{
		using type = std::function<void()>;
	};

	template<typename T, typename U>
	__forceinline bool Equals(const T& lhs, const U& rhs)
	{
		return lhs == rhs;
	}

	template<typename T, typename... U>
	__forceinline size_t GetAddress(const std::function<T(U...)>& f)
	{
		typedef T(fnType)(U...);
		fnType** fnPointer = (const_cast<std::function<T(U...)>&>(f)).template target<fnType*>();
		return (size_t)*fnPointer;
	}

	template<typename T, typename... U>
	__forceinline bool Equals(const std::function<T(U...)>& lhs, const std::function<T(U...)>& rhs)
	{
		return GetAddress(lhs) == GetAddress(rhs);
	}

}