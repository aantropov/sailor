#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include <iterator>
#include "Defines.h"
#include "Math/Math.h"
#include "Memory/Memory.h"

namespace Sailor
{
	template<typename T>
	concept IsTriviallyDestructible = std::is_trivially_destructible<T>::value;

	template<typename T>
	using TPredicate = std::function<bool(const T&)>;

	template<typename TElementType, typename TAllocator = Memory::MallocAllocator>
	class SAILOR_API TVector
	{
	public:

		template<typename TElementType>
		class SAILOR_API TIterator
		{
		public:

			using iterator_category = std::random_access_iterator_tag;
			using value_type = TElementType;
			using difference_type = int64_t;
			using pointer = TElementType*;
			using reference = TElementType&;

			TIterator() : m_element(nullptr) {}

			TIterator(const TIterator&) = default;
			TIterator(TIterator&&) = default;

			virtual ~TIterator() = default;

			TIterator(pointer element) : m_element(element) {}

			TIterator& operator=(const TIterator& rhs)
			{
				m_element = rhs.m_element;
				return *this;
			}

			TIterator& operator=(TIterator&& rhs)
			{
				m_element = rhs.m_element;
				rhs.m_element = nullptr;
			}

			bool operator==(const TIterator& rhs) const { return m_element == rhs.m_element; }
			bool operator!=(const TIterator& rhs) const { return m_element != rhs.m_element; }

			bool operator<(const TIterator& rhs) const { return *m_element < *rhs.m_element; }

			reference operator*() { return *m_element; }
			reference operator*() const { return *m_element; }

			reference operator->() { return m_element; }
			reference operator->() const { return m_element; }

			TIterator& operator++()
			{
				++m_element;
				return *this;
			}

			TIterator& operator--()
			{
				--m_element;
				return *this;
			}

			TIterator operator-(difference_type rhs) const { return m_element - rhs; }
			TIterator operator+(difference_type rhs) const { return m_element + rhs; }

			TIterator& operator-=(difference_type rhs) { m_element -= rhs; return this; }
			TIterator& operator+=(difference_type rhs) { m_element += rhs; return this; }

			difference_type operator-(const TIterator& other) const
			{
				return (difference_type)(m_element - other.m_element);
			}

			static friend TIterator<TElementType> operator-(const difference_type& offset, TIterator<TElementType>& other)
			{
				return other - offset;
			}

			static friend TIterator<TElementType> operator+(const difference_type& offset, TIterator<TElementType>& other)
			{
				return other + offset;
			}

		protected:

			pointer m_element;
		};

		template<typename TElementType>
		using TConstIterator = TIterator<const TElementType>;

		static constexpr size_t InvalidIndex = (size_t)-1;

		// Constructors & Destructor
		TVector() : m_arrayNum(0), m_capacity(0) {}
		TVector(size_t countDefaultElements)
		{
			AddDefault(countDefaultElements);
		}

		virtual ~TVector() { Clear(); }

		TVector(std::initializer_list<TElementType> initList) : TVector(initList.begin(), initList.size()) {}

		TVector(const TVector& other) : TVector(other.GetData(), other.Num()) {}

		TVector(TVector&& other) { Swap(*this, other); }

		TVector(const TElementType* rawPtr, size_t count)
		{
			ResizeIfNeeded(count);

			m_arrayNum = count;
			ConstructElements(0, rawPtr[0], count);
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

		TVector& operator=(TVector&& other)
		{
			Swap(*this, other);
			return *this;
		}

		TVector& operator=(const TVector& other)
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

		void AddRange(const TElementType* first, size_t count)
		{
			if (count == 0)
			{
				return;
			}

			ResizeIfNeeded(m_arrayNum + count);

			ConstructElements(m_arrayNum, *first, count);
			m_arrayNum += count;
		}

		void AddRange(const TVector& other)
		{
			AddRange(other.GetData(), other.Num());
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
				if (m_pRawPtr[i] == item)
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
			memmove(&m_pRawPtr[index], &m_pRawPtr[index + count], sizeof(TElementType) * (m_arrayNum - index - count));
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

		bool IsEmpty() const { return m_arrayNum == 0; }

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

		void Swap(TVector& lhs, TVector& rhs) const
		{
			std::swap(lhs.m_pRawPtr, rhs.m_pRawPtr);
			std::swap(lhs.m_allocator, rhs.m_allocator);
			std::swap(lhs.m_arrayNum, rhs.m_arrayNum);
			std::swap(lhs.m_capacity, rhs.m_capacity);
		}

		// Support ranged for
		TIterator<TElementType> begin() { return TIterator<TElementType>(m_pRawPtr); }
		TIterator<TElementType> end() { return TIterator<TElementType>(m_pRawPtr + m_arrayNum); }

		TConstIterator<TElementType> begin() const { return TConstIterator<TElementType>(m_pRawPtr); }
		TConstIterator<TElementType> end() const { return TConstIterator<TElementType>(m_pRawPtr + m_arrayNum); }

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

			new (&m_pRawPtr[index]) TElementType(std::forward<TArgs>(args)...);
			return m_arrayNum++;
		}

		TAllocator m_allocator{};
		size_t m_arrayNum = 0;
		size_t m_capacity = 0;

		TElementType* m_pRawPtr = nullptr;

		void ConstructElements(size_t index, size_t count = 1)
		{
			for (size_t i = 0; i < count; i++)
			{
				new (&m_pRawPtr[i + index]) TElementType();
			}
		}

		void ConstructElements(size_t index, const TElementType& firstElement, size_t count = 1)
		{
			for (size_t i = 0; i < count; i++)
			{
				new (&m_pRawPtr[i + index]) TElementType((&firstElement)[i]);
			}
		}

		void DestructElements(size_t index, size_t count = 1) requires !IsTriviallyDestructible<TElementType>
		{
			for (size_t i = 0; i < count; i++)
			{
				m_pRawPtr[index + i].~TElementType();
			}
		}

		void DestructElements(size_t index, size_t count = 1) requires IsTriviallyDestructible<TElementType>
		{
			// Do nothing for trivial types
		}
	};

	SAILOR_API void RunVectorBenchmark();
}