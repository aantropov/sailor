#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include <iterator>
#include <algorithm>
#include "Defines.h"
#include "Math/Math.h"
#include "Memory/MallocAllocator.h"

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

	template<typename TDataType>
	class SAILOR_API TVectorIterator
	{
	public:

		using iterator_category = std::random_access_iterator_tag;
		using value_type = TDataType;
		using difference_type = int64_t;
		using pointer = TDataType*;
		using reference = TDataType&;

		TVectorIterator() : m_element(nullptr) {}

		TVectorIterator(const TVectorIterator&) = default;
		TVectorIterator(TVectorIterator&&) = default;

		~TVectorIterator() = default;

		TVectorIterator(pointer element) : m_element(element) {}

		TVectorIterator& operator=(const TVectorIterator& rhs)
		{
			m_element = rhs.m_element;
			return *this;
		}

		TVectorIterator& operator=(TVectorIterator&& rhs)
		{
			m_element = rhs.m_element;
			rhs.m_element = nullptr;
			return *this;
		}

		bool operator==(const TVectorIterator& rhs) const { return m_element == rhs.m_element; }
		bool operator!=(const TVectorIterator& rhs) const { return m_element != rhs.m_element; }

		bool operator<(const TVectorIterator& rhs) const { return *m_element < *rhs.m_element; }

		reference operator*() { return *m_element; }
		reference operator*() const { return *m_element; }

		reference operator->() { return m_element; }
		reference operator->() const { return m_element; }

		TVectorIterator& operator++()
		{
			++m_element;
			return *this;
		}

		TVectorIterator& operator--()
		{
			--m_element;
			return *this;
		}

		TVectorIterator operator-(difference_type rhs) const { return m_element - rhs; }
		TVectorIterator operator+(difference_type rhs) const { return m_element + rhs; }

		TVectorIterator& operator-=(difference_type rhs) { m_element -= rhs; return this; }
		TVectorIterator& operator+=(difference_type rhs) { m_element += rhs; return this; }

		difference_type operator-(const TVectorIterator& other) const
		{
			return (difference_type)(m_element - other.m_element);
		}

		static friend TVectorIterator<TDataType> operator-(const difference_type& offset, TVectorIterator<TDataType>& other)
		{
			return other - offset;
		}

		static friend TVectorIterator<TDataType> operator+(const difference_type& offset, TVectorIterator<TDataType>& other)
		{
			return other + offset;
		}

	protected:

		pointer m_element;
	};

	template<typename TDataType>
	using TConstVectorIterator = TVectorIterator<const TDataType>;

	template<typename TElementType, typename TAllocator = Memory::MallocAllocator>
	class SAILOR_API TVector final
	{
	public:

		static constexpr size_t InvalidIndex = (size_t)-1;

		// Constructors & Destructor
		TVector() : m_arrayNum(0), m_capacity(0) {}
		TVector(size_t countDefaultElements)
		{
			AddDefault(countDefaultElements);
		}

		~TVector() { Clear(); }

		TVector(std::initializer_list<TElementType> initList) : TVector(initList.begin(), initList.size()) {}

		TVector(const TVector& other) requires IsCopyConstructible<TElementType> : TVector(other.GetData(), other.Num()) {}

		TVector(TVector&& other) noexcept requires IsMoveConstructible<TAllocator> { Swap(*this, other); }

		TVector(const TElementType* rawPtr, size_t count) requires IsCopyConstructible<TElementType>
		{
			ResizeIfNeeded(count);

			m_arrayNum = count;
			ConstructElements(0, rawPtr[0], count);
		}

		TVector(TElementType* rawPtr, size_t count) requires IsMoveConstructible<TElementType> && !IsCopyConstructible<TElementType>
		{
			ResizeIfNeeded(count);

			m_arrayNum = count;
			MoveElements(0, rawPtr[0], count);
		}

		// Operators
		__forceinline const TElementType& operator[](size_t index) const
		{
			assert(index < m_arrayNum);
			return m_pRawPtr[index];
		}

		__forceinline TElementType& operator[](size_t index)
		{
			assert(index < m_arrayNum);
			return m_pRawPtr[index];
		}

		__forceinline bool operator!=(const TVector& otherArray) const { return !(otherArray == this); }
		__forceinline bool operator==(const TVector& otherArray) const
		{
			if (m_arrayNum != otherArray.m_arrayNum)
			{
				return false;
			}

			for (size_t i = 0; i < m_arrayNum; i++)
			{
				if (otherArray[i] != operator[](i))
				{
					return false;
				}
			}
			return true;
		}

		TVector& operator=(std::initializer_list<TElementType> initList)
		{
			Clear(false);
			AddRange(initList);
			return *this;
		}

		TVector& operator=(TVector&& other) requires IsMoveConstructible<TAllocator>
		{
			Swap(*this, other);
			return *this;
		}

		TVector& operator=(const TVector& other) requires IsCopyConstructible<TElementType>
		{
			Clear(false);
			AddRange(other);
			return *this;
		}

		// Methods

		template<typename... TArgs>
		size_t Emplace(TArgs&& ... args)
		{
			return EmplaceAt(m_arrayNum, args ...);
		}

		void Resize(size_t count)
		{
			if (m_arrayNum > count)
			{
				DestructElements(count, m_arrayNum - count);
				m_arrayNum = count;
				return;
			}

			ResizeIfNeeded(count);
			ConstructElements(m_arrayNum, count - m_arrayNum);
			m_arrayNum = count;
		}

		void AddDefault(size_t count)
		{
			ResizeIfNeeded(m_arrayNum + count);

			ConstructElements(m_arrayNum, count);
			m_arrayNum += count;
		}

		size_t Add(TElementType item)
		{
			return Emplace(item);
		}

		void MoveRange(TElementType* first, size_t count) requires IsMoveConstructible<TElementType>
		{
			if (count == 0)
			{
				return;
			}

			ResizeIfNeeded(m_arrayNum + count);

			MoveElements(m_arrayNum, *first, count);
			m_arrayNum += count;
		}

		void AddRange(const TElementType* first, size_t count) requires IsCopyConstructible<TElementType>
		{
			if (count == 0)
			{
				return;
			}

			ResizeIfNeeded(m_arrayNum + count);

			ConstructElements(m_arrayNum, *first, count);
			m_arrayNum += count;
		}

		void AddRange(const TVector& other) requires IsCopyConstructible<TElementType>
		{
			AddRange(other.GetData(), other.Num());
		}

		void AddRange(TVector&& other) requires IsMoveConstructible<TElementType>
		{
			MoveRange(other.GetData(), other.Num());
		}

		void AddRange(std::initializer_list<TElementType> initList)
		{
			AddRange(initList.begin(), initList.size());
		}

		bool Contains(const TElementType& item) const
		{
			for (size_t i = 0; i < m_arrayNum; i++)
			{
				if (m_pRawPtr[i] == item)
				{
					return true;
				}
			}

			return false;
		}

		bool ContainsIf(const TPredicate<TElementType>& predicate) const
		{
			for (size_t i = 0; i < m_arrayNum; i++)
			{
				if (predicate(m_pRawPtr[i]))
				{
					return true;
				}
			}

			return false;
		}

		size_t Find(const TElementType& item) const
		{
			for (size_t i = 0; i < m_arrayNum; i++)
			{
				if (item == m_pRawPtr[i])
				{
					return i;
				}
			}
			return -1;
		}

		size_t FindLast(const TElementType& item) const
		{
			for (int32 i = m_arrayNum - 1; i >= 0; i--)
			{
				if (m_pRawPtr[i] == item)
				{
					return i;
				}
			}
			return -1;
		}

		size_t FindIf(const TPredicate<TElementType>& predicate) const
		{
			for (size_t i = 0; i < m_arrayNum; i++)
			{
				if (predicate(m_pRawPtr[i]))
				{
					return i;
				}
			}
			return -1;
		}

		void Insert(const TElementType& item, size_t index)
		{
			ResizeIfNeeded(m_arrayNum + 1);

			if (index == m_arrayNum)
			{
				Add(item);
				return;
			}

			memmove(&m_pRawPtr[index + 1], &m_pRawPtr[index], (m_arrayNum - index) * sizeof(TElementType));

			ConstructElements(index, item, 1);
			m_arrayNum++;
		}

		void Insert(TElementType&& item, size_t index)
		{
			if (index == m_arrayNum)
			{
				Emplace(item);
				return;
			}

			memmove(&m_pRawPtr[index + 1], &m_pRawPtr[index], (m_arrayNum - index) * sizeof(TElementType));
			EmplaceAt(index, item);
		}

		void Insert(const TVector& vector, size_t index)
		{
			Insert(vector.GetData(), vector.Num(), index);
		}

		void Insert(std::initializer_list<TElementType> initList, size_t index)
		{
			Insert(initList.begin(), initList.size(), index);
		}

		void Insert(TElementType* first, size_t count, size_t index)
		{
			if (count == 0)
			{
				return;
			}

			ResizeIfNeeded(m_arrayNum + count);

			if (index == m_arrayNum)
			{
				AddRange(first, count);
				return;
			}

			memmove(&m_pRawPtr[index + count], &m_pRawPtr[index], (m_arrayNum - index) * sizeof(TElementType));

			ConstructElements(index, first, count);
			m_arrayNum += count;
		}

		__forceinline bool IsValidIndex(size_t index) const { return 0 < index && index < m_arrayNum; }

		__forceinline size_t Num() const { return m_arrayNum; }
		__forceinline size_t Capacity() const { return m_capacity; }

		void RemoveLast()
		{
			DestructElements(m_arrayNum - 1, 1);
			m_arrayNum--;
		}

		size_t Remove(const TElementType& item)
		{
			return RemoveAll([&](const auto& el) { return el == item; });
		}

		size_t RemoveAll(const TPredicate<TElementType>& predicate)
		{
			size_t shift = 0;
			for (size_t i = 0; i < m_arrayNum - shift; i++)
			{
				const bool bShouldDelete = predicate(m_pRawPtr[i]);
				if (bShouldDelete)
				{
					shift++;
					DestructElements(i, 1);
				}

				if (shift)
				{
					memmove(&m_pRawPtr[i], &m_pRawPtr[i + shift], sizeof(TElementType));
				}

				if (bShouldDelete)
				{
					i--;
				}
			}

			m_arrayNum -= shift;
			return shift;
		}

		void RemoveAt(size_t index, size_t count = 1)
		{
			DestructElements(index, count);
			if (m_arrayNum - count != 0)
			{
				memmove(&m_pRawPtr[index], &m_pRawPtr[index + count], sizeof(TElementType) * (m_arrayNum - index - count));
			}

			m_arrayNum -= count;
		}

		void RemoveAtSwap(size_t index, size_t count = 1)
		{
			DestructElements(index, count);
			memmove(&m_pRawPtr[index], &m_pRawPtr[m_arrayNum - count], sizeof(TElementType) * count);
			m_arrayNum -= count;
		}

		size_t RemoveFirst(const TElementType& item)
		{
			for (size_t i = 0; i < m_arrayNum; i++)
			{
				if (const bool bShouldDelete = m_pRawPtr[i] == item)
				{
					DestructElements(i, 1);
					memmove(&m_pRawPtr[i], &m_pRawPtr[i + 1], (m_arrayNum - i - 1) * sizeof(TElementType));
					return m_arrayNum--;
				}
			}

			return -1;
		}

		void Clear(bool bResetCapacity = true)
		{
			DestructElements(0, m_arrayNum);

			m_arrayNum = 0;
			if (bResetCapacity)
			{
				m_capacity = 0;
				m_allocator.Free(m_pRawPtr);
				m_pRawPtr = nullptr;
			}
		}

		void Reserve(size_t newCapacity)
		{
			assert(newCapacity > m_capacity);
			const size_t slack = newCapacity - m_capacity;

			m_capacity = newCapacity;

			if (m_capacity)
			{
				if (m_pRawPtr)
				{
					m_pRawPtr = static_cast<TElementType*>(m_allocator.Reallocate(m_pRawPtr, newCapacity * sizeof(TElementType)));
				}
				else
				{
					m_pRawPtr = static_cast<TElementType*>(m_allocator.Allocate(newCapacity * sizeof(TElementType)));
				}
			}
		}

		__forceinline bool IsEmpty() const { return m_arrayNum == 0; }

		void Sort()
		{
			// For now use std
			std::stable_sort(begin(), end());
		}

		void Sort(const TPredicate<TElementType>& predicate)
		{
			// For now use std
			std::stable_sort(begin(), end(), predicate);
		}

		static void Swap(TVector& lhs, TVector& rhs)
		{
			std::swap(lhs.m_pRawPtr, rhs.m_pRawPtr);
			std::swap(lhs.m_allocator, rhs.m_allocator);
			std::swap(lhs.m_arrayNum, rhs.m_arrayNum);
			std::swap(lhs.m_capacity, rhs.m_capacity);
		}

		// Support ranged for
		TVectorIterator<TElementType> begin() { return TVectorIterator<TElementType>(m_pRawPtr); }
		TVectorIterator<TElementType> end() { return TVectorIterator<TElementType>(m_pRawPtr + m_arrayNum); }

		TConstVectorIterator<TElementType> begin() const { return TConstVectorIterator<TElementType>(m_pRawPtr); }
		TConstVectorIterator<TElementType> end() const { return TConstVectorIterator<TElementType>(m_pRawPtr + m_arrayNum); }

		TElementType* GetData() { return m_pRawPtr; }
		const TElementType* GetData() const { return m_pRawPtr; }

	protected:

		__forceinline void ResizeIfNeeded(size_t newSize)
		{
			if (m_capacity < newSize)
			{
				const size_t optimalSize = Math::UpperPowOf2((uint32_t)(newSize));
				Reserve(optimalSize);
			}
		}

		template<typename... TArgs>
		__forceinline size_t EmplaceAt(size_t index, TArgs&& ... args)
		{
			ResizeIfNeeded(m_arrayNum + 1);

			new (&m_pRawPtr[index]) TElementType(std::move<TArgs>(args)...);
			return m_arrayNum++;
		}

		TElementType* m_pRawPtr = nullptr;
		size_t m_arrayNum = 0;
		size_t m_capacity = 0;
		TAllocator m_allocator{};

		__forceinline void ConstructElements(size_t index, size_t count = 1)
		{
			for (size_t i = 0; i < count; i++)
			{
				new (&m_pRawPtr[i + index]) TElementType();
			}
		}

		__forceinline void ConstructElements(size_t index, const TElementType& firstElement, size_t count = 1) requires IsCopyConstructible<TElementType>
		{
			for (size_t i = 0; i < count; i++)
			{
				new (&m_pRawPtr[i + index]) TElementType((&firstElement)[i]);
			}
		}

		__forceinline void MoveElements(size_t index, TElementType& firstElement, size_t count = 1) requires IsMoveConstructible<TElementType>
		{
			for (size_t i = 0; i < count; i++)
			{
				new (&m_pRawPtr[i + index]) TElementType(std::move((&firstElement)[i]));
			}
		}

		__forceinline void DestructElements(size_t index, size_t count = 1) requires !IsTriviallyDestructible<TElementType>
		{
			for (size_t i = 0; i < count; i++)
			{
				m_pRawPtr[index + i].~TElementType();
			}
		}

		__forceinline void DestructElements(size_t index, size_t count = 1) requires IsTriviallyDestructible<TElementType>
		{
			// Do nothing for trivial types
		}
	};

	SAILOR_API void RunVectorBenchmark();
}