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

		TVectorIterator(const TVectorIterator& rhs) : m_element(rhs.m_element) {}

		TVectorIterator(TVectorIterator&& rhs) noexcept
		{
			m_element = rhs.m_element;
			rhs.m_element = nullptr;
		}

		~TVectorIterator() = default;

		TVectorIterator(pointer element) : m_element(element) {}

		TVectorIterator& operator=(const TVectorIterator& rhs)
		{
			m_element = rhs.m_element;
			return *this;
		}

		TVectorIterator& operator=(TVectorIterator&& rhs)  noexcept
		{
			m_element = rhs.m_element;
			rhs.m_element = nullptr;
			return *this;
		}

		operator TVectorIterator<const TDataType>() const { return TVectorIterator<const TDataType>(m_element); }

		bool operator==(const TVectorIterator& rhs) const { return m_element == rhs.m_element; }
		bool operator!=(const TVectorIterator& rhs) const { return m_element != rhs.m_element; }

		bool operator<(const TVectorIterator& rhs) const { return m_element < rhs.m_element; }
		bool operator>(const TVectorIterator& rhs) const { return m_element > rhs.m_element; }
		bool operator<=(const TVectorIterator& rhs) const { return m_element <= rhs.m_element; }
		bool operator>=(const TVectorIterator& rhs) const { return m_element >= rhs.m_element; }

		reference operator*() { return *m_element; }
		reference operator*() const { return *m_element; }

		pointer operator->() { return m_element; }
		pointer operator->() const { return m_element; }

		TVectorIterator& operator++()
		{
			++m_element;
			return *this;
		}

		TVectorIterator operator++(int)
		{
			TVectorIterator previous(*this);
			++(*this);
			return previous;
		}

		TVectorIterator& operator--()
		{
			--m_element;
			return *this;
		}

		TVectorIterator operator--(int)
		{
			TVectorIterator previous(*this);
			--(*this);
			return previous;
		}

		TVectorIterator operator-(difference_type rhs) const { return m_element - rhs; }
		TVectorIterator operator+(difference_type rhs) const { return m_element + rhs; }
		reference operator[](difference_type rhs) const { return m_element[rhs]; }

		TVectorIterator& operator-=(difference_type rhs) { m_element -= rhs; return *this; }
		TVectorIterator& operator+=(difference_type rhs) { m_element += rhs; return *this; }

		difference_type operator-(const TVectorIterator& other) const
		{
			return (difference_type)(m_element - other.m_element);
		}

		friend TVectorIterator<TDataType> operator+(difference_type offset, const TVectorIterator<TDataType>& other)
		{
			return other + offset;
		}

	protected:

		pointer m_element;
	};

	template<typename TDataType>
	using TConstVectorIterator = TVectorIterator<const TDataType>;

	template<typename TElementType, typename TAllocator = Memory::DefaultGlobalAllocator>
	class SAILOR_API TVector final
	{
	public:

		using TIterator = TVectorIterator<TElementType>;
		using TConstIterator = TConstVectorIterator<const TElementType>;

		static constexpr size_t InvalidIndex = (size_t)-1;

		// Constructors & Destructor
		TVector() : m_arrayNum(0), m_capacity(0) {}
		TVector(size_t countDefaultElements)
		{
			AddDefault(countDefaultElements);
		}

		~TVector() { Clear(true); }

		TVector(std::initializer_list<TElementType> initList) : TVector(initList.begin(), initList.size()) {}

		TVector(const TVector& other) requires IsCopyConstructible<TElementType> : TVector(other.GetData(), other.Num()) {}

		template<typename TElementType1>
		TVector(const TVector<TElementType1>& other) requires IsCopyConstructible<TElementType1>
		{
			Reserve(other.Num());

			for (uint32_t i = 0; i < other.Num(); i++)
			{
				Add(static_cast<TElementType>(other[i]));
			}
		}

		TVector(TVector&& other) noexcept requires IsMoveConstructible<TAllocator> { Swap(*this, other); }

		TVector(const TElementType* rawPtr, size_t count) requires IsCopyConstructible<TElementType>
		{
			AddRange(rawPtr, count);
		}

		TVector(TElementType* rawPtr, size_t count) requires IsMoveConstructible<TElementType> && (!IsCopyConstructible<TElementType>)
		{
			MoveRange(rawPtr, count);
		}

		TVector(size_t size, const TElementType& defaultEl)
		{
			Reserve(size);

			for (size_t i = 0; i < size; i++)
			{
				Emplace(defaultEl);
			}
		}

		// Operators
		__forceinline const TElementType& operator[](size_t index) const
		{
			check(index < m_arrayNum);
			return m_pRawPtr[index];
		}

		__forceinline TElementType& operator[](size_t index)
		{
			check(index < m_arrayNum);
			return m_pRawPtr[index];
		}

		template<typename TElementType1>
		TVector<TElementType1> Select(const std::function<TElementType1(const TElementType&)>& func) const
		{
			TVector<TElementType1> res;
			res.Reserve(m_arrayNum);

			for (uint32_t i = 0; i < m_arrayNum; i++)
			{
				res.Add(func(m_pRawPtr[i]));
			}

			return res;
		}

		template<typename TAllocator1>
		__forceinline bool operator==(const TVector<TElementType, TAllocator1>& otherArray) const
		{
			if (m_arrayNum != otherArray.m_arrayNum)
			{
				return false;
			}

			for (size_t i = 0; i < m_arrayNum; i++)
			{
				if (!(otherArray[i] == operator[](i)))
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

		TVector& operator=(TVector&& other) noexcept requires IsMoveConstructible<TAllocator>
		{
			Swap(*this, other);
			return *this;
		}

		TVector& operator=(const TVector& other) requires IsCopyConstructible<TElementType>
		{
			if (this != &other)
			{
				Clear(false);
				AddRange(other);
			}
			return *this;
		}

		// Methods

		template<typename... TArgs>
		size_t Emplace(TArgs&& ... args)
		{
			return EmplaceAt(m_arrayNum, std::forward<TArgs>(args) ...);
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
			if constexpr (IsMoveConstructible<TElementType>)
			{
				return Emplace(std::move(item));
			}
			else
			{
				return Emplace(item);
			}
		}

		size_t AddUnique(TElementType item)
		{
			for (size_t i = 0; i < m_arrayNum; i++)
			{
				if (m_pRawPtr[i] == item)
				{
					return i;
				}
			}

			if constexpr (IsMoveConstructible<TElementType>)
			{
				return Emplace(std::move(item));
			}
			else
			{
				return Emplace(item);
			}
		}

		void MoveRange(TElementType* first, size_t count) requires IsMoveConstructible<TElementType>
		{
			if (count == 0)
			{
				return;
			}

			ResizeIfNeeded(m_arrayNum + count);

			ConstructMoveElements(m_arrayNum, *first, count);
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

			MemMove(index + 1, index, m_arrayNum - index);

			ConstructElements(index, item, 1);
			m_arrayNum++;
		}

		void Insert(TElementType&& item, size_t index)
		{
			// No need to resize since it is handled in Emplace functions
			if (index == m_arrayNum)
			{
				Emplace(std::forward<TElementType>(item));
				return;
			}

			ResizeIfNeeded(m_arrayNum + 1);
			MemMove(index + 1, index, m_arrayNum - index);
			EmplaceAt(index, std::forward<TElementType>(item));
		}

		void Insert(const TVector& vector, size_t index)
		{
			Insert(vector.GetData(), vector.Num(), index);
		}

		void Insert(std::initializer_list<TElementType> initList, size_t index)
		{
			Insert(initList.begin(), initList.size(), index);
		}

		void Insert(const TElementType* first, size_t count, size_t index)
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

			MemMove(index + count, index, m_arrayNum - index);
			ConstructElements(index, *first, count);
			m_arrayNum += count;
		}

		__forceinline bool IsValidIndex(size_t index) const { return index < m_arrayNum; }

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
			size_t num = 0;

			if constexpr (IsMoveConstructible<TAllocator>)
			{
				TVector<TElementType> tmp;
				tmp.Reserve(m_arrayNum);

				for (size_t i = 0; i < m_arrayNum; i++)
				{
					if (predicate(m_pRawPtr[i]))
					{
						num++;
						continue;
					}

					if constexpr (IsMoveConstructible<TElementType>)
					{
						tmp.Emplace(std::move(m_pRawPtr[i]));
					}
					else
					{
						tmp.Add(m_pRawPtr[i]);
					}
				}

				(*this) = std::move(tmp);
			}
			else
			{
				for (size_t i = 0; i < m_arrayNum; i++)
				{
					if (predicate(m_pRawPtr[i]))
					{
						num++;
						RemoveAt(i);
						i--;
					}
				}
			}

			return num;
		}

		void RemoveAt(size_t index, size_t count = 1)
		{
			check(index <= m_arrayNum);
			check(count <= m_arrayNum - index);
			if (count == 0)
			{
				return;
			}

			DestructElements(index, count);
			MemMove(index, index + count, m_arrayNum - index - count);
			m_arrayNum -= count;
		}

		void RemoveAtSwap(size_t index, size_t count = 1)
		{
			check(index <= m_arrayNum);
			check(count <= m_arrayNum - index);
			if (count == 0)
			{
				return;
			}

			const size_t numToMove = std::min(count, m_arrayNum - index - count);
			DestructElements(index, count);
			MemMove(index, m_arrayNum - numToMove, numToMove);
			m_arrayNum -= count;
		}

		size_t RemoveFirst(const TElementType& item)
		{
			for (size_t i = 0; i < m_arrayNum; i++)
			{
				if (m_pRawPtr[i] == item)
				{
					const size_t previousNum = m_arrayNum;
					RemoveAt(i);
					return previousNum;
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
			if (newCapacity <= m_capacity)
			{
				return;
			}

			m_capacity = newCapacity;

			if (m_pRawPtr && m_allocator.Reallocate(m_pRawPtr, newCapacity * sizeof(TElementType), alignof(TElementType)))
			{
				return;
			}

			TElementType* pRawPtr = static_cast<TElementType*>(m_allocator.Allocate(newCapacity * sizeof(TElementType), alignof(TElementType)));
			std::swap(m_pRawPtr, pRawPtr);

			if (m_arrayNum > 0)
			{
				if constexpr (IsMoveConstructible<TElementType>)
				{
					ConstructMoveElements(0, pRawPtr[0], m_arrayNum);
				}
				else if constexpr (IsCopyConstructible<TElementType>)
				{
					ConstructElements(0, pRawPtr[0], m_arrayNum);
				}
				else
				{
					// No way to save elements
					check(false);
				}

				// Destruct old elements
				for (size_t i = 0; i < m_arrayNum; i++)
				{
					pRawPtr[i].~TElementType();
				}
			}

			m_allocator.Free(pRawPtr);
		}

		__forceinline bool IsEmpty() const { return m_arrayNum == 0; }

		void Sort()
		{
			// For now use std
			std::stable_sort(begin(), end());
		}

		void Sort(const TCompare<TElementType>& compare)
		{
			// For now use std
			std::stable_sort(begin(), end(), compare);
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

		TVectorIterator<TElementType>  First() { return begin(); }
		TConstVectorIterator<TElementType> First() const { return begin(); }

		TVectorIterator<TElementType>  Last()
		{
			if (m_arrayNum == 0 || m_pRawPtr == nullptr)
			{
				return end();
			}

			return TVectorIterator<TElementType>(m_pRawPtr + m_arrayNum - 1);
		}

		TConstVectorIterator<TElementType> Last() const
		{
			if (m_arrayNum == 0 || m_pRawPtr == nullptr)
			{
				return end();
			}

			return TConstVectorIterator<TElementType>(m_pRawPtr + m_arrayNum - 1);
		}

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
			m_arrayNum++;
			return index;
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

		__forceinline void ConstructElements(size_t index, const TElementType& firstElement, size_t count) requires IsCopyConstructible<TElementType>
		{
			for (size_t i = 0; i < count; i++)
			{
				new (&m_pRawPtr[i + index]) TElementType((&firstElement)[i]);
			}
		}

		__forceinline void ConstructMoveElements(size_t index, TElementType& firstElement, size_t count = 1) requires IsMoveConstructible<TElementType>
		{
			for (size_t i = 0; i < count; i++)
			{
				new (&m_pRawPtr[i + index]) TElementType(std::move((&firstElement)[i]));
			}
		}

		__forceinline void DestructElements(size_t index, size_t count = 1) requires (!IsTriviallyDestructible<TElementType>)
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

		__forceinline void MemMove(size_t to, size_t from, size_t count)
		{
			if (count == 0 || to == from)
			{
				return;
			}

			if constexpr (IsTriviallyCopyable<TElementType>)
			{
				memmove(&m_pRawPtr[to], &m_pRawPtr[from], count * sizeof(TElementType));
			}
			else
			{
				for (size_t i = 0; i < count; i++)
				{
					const size_t shift = from < to ? (count - i - 1) : i;
					if constexpr (IsMoveConstructible<TElementType>)
					{
						ConstructMoveElements(to + shift, m_pRawPtr[from + shift], 1);
					}
					else
					{
						ConstructElements(to + shift, m_pRawPtr[from + shift], 1);
					}
					DestructElements(from + shift, 1);
				}
			}
		}

		friend class TVector;
	};

	SAILOR_API void RunVectorBenchmark();
}
