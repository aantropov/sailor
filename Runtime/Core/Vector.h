#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include "Defines.h"
#include "Math/Math.h"
#include "Memory/Memory.h"

namespace Sailor
{
	template<typename TElementType>
	struct SAILOR_API TIterator
	{
		TElementType* m_element;
		TIterator(TElementType* element) : m_element(element) {}

		bool operator!=(TIterator rhs) const { return m_element != rhs.m_element; }
		TElementType& operator*() { return *m_element; }
		void operator++() { ++m_element; }
	};

	template<typename TElementType>
	struct SAILOR_API TConstIterator
	{
		const TElementType* m_element;
		TConstIterator(const TElementType* element) : m_element(element) {}

		bool operator!=(TConstIterator rhs) const { return m_element != rhs.m_element; }
		const TElementType& operator*() const { return *m_element; }
		void operator++() { ++m_element; }
	};

	template<typename T>
	concept IsTriviallyDestructible = std::is_trivially_destructible<T>::value;

	template<typename T>
	using TPredicate = std::function<bool(const T&)>;

	template<typename TElementType, typename TAllocator = Memory::GlobalHeapAllocator>
	class SAILOR_API TVector
	{

	public:

		// Constructors & Destructor
		TVector() : m_arrayNum(0), m_capacity(0) {}
		virtual ~TVector() { Clear(); }

		TVector(std::initializer_list<TElementType> initList) : TVector(initList.begin(), initList.size()) {}

		TVector(const TVector& other) : TVector(other[0], other.Num()) {}

		TVector(TVector&& other) { Swap(this, other); }

		TVector(const TElementType* rawPtr, size_t count)
		{
			ResizeIfNeeded(count);

			m_arrayNum = count;
			ConstructElements(0, rawPtr[0], count);
		}

		// Operators
		const TElementType& operator[](size_t index) const { return m_pRawPtr[index]; }
		TElementType& operator[](size_t index) { return m_pRawPtr[index]; }

		bool operator!=(const TVector& otherArray) const { return !(otherArray == this); }
		bool operator==(const TVector& otherArray) const
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
		}

		TVector& operator=(TVector&& other)
		{
			Swap(this, other);
			return this;
		}

		TVector& operator=(const TVector& other)
		{
			Clear(false);
			AddRange(other);
			return this;
		}

		// Methods

		template<typename... TArgs>
		size_t Emplace(TArgs&& ... args)
		{
			ResizeIfNeeded(1);

			new (&m_pRawPtr[m_arrayNum]) TElementType(std::forward<TArgs>(args)...);
			return m_arrayNum++;
		}

		size_t Add(const TElementType& item)
		{
			return Emplace(item);
		}

		size_t Add(TElementType&& item)
		{
			ResizeIfNeeded(1);

			std::swap(m_pRawPtr[m_arrayNum], item);

			return m_arrayNum++;
		}

		void AddRange(TVector&& other)
		{
			if (m_capacity == 0 && !m_pRawPtr)
			{
				Swap(this, other);
			}
			else
			{
				AddRange(other);
			}
		}

		void AddRange(const TElementType* first, size_t count)
		{
			if (count == 0)
			{
				return;
			}

			ResizeIfNeeded(count);

			ConstructElements(m_arrayNum, *first, count);
			m_arrayNum += count;
		}

		void AddRange(const TVector& other)
		{
			AddRange(other[0], other.Num());
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
			if (m_arrayNum == 0)
			{
				return -1;
			}

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
			ResizeIfNeeded(1);

			if (index == m_arrayNum)
			{
				Add(item);
				return;
			}

			for (int32 i = m_arrayNum + 1; i > index; i--)
			{
				m_pRawPtr[i] == m_pRawPtr[i - 1];
			}

			ConstructElements(index, item, 1);
			m_arrayNum++;
		}

		void Insert(const TVector& vector, size_t index)
		{
			Insert(vector[0], vector.Num(), index);
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

			ResizeIfNeeded(count);

			if (index == m_arrayNum)
			{
				AddRange(first);
				return;
			}

			for (int32 i = m_arrayNum + count; i > index + count; i--)
			{
				m_pRawPtr[i] == m_pRawPtr[i - count];
			}

			ConstructElements(index, first, count);
			m_arrayNum += count;
		}

		bool IsValidIndex(size_t index) const { return 0 < index && index < m_arrayNum; }
		size_t Num() const { return m_arrayNum; }

		void RemoveLast()
		{
			DestructElements(m_arrayNum - 1, 1);
			m_arrayNum--;
		}

		size_t Remove(const TElementType& item)
		{
			size_t shift = 0;
			for (size_t i = 0; i < m_arrayNum - shift; i++)
			{
				if (m_pRawPtr[i] == item)
				{
					i--;
					shift++;
				}

				if (shift)
				{
					DestructElements(i, 1);
					m_pRawPtr[i] = m_pRawPtr[i + shift];
				}
			}

			m_arrayNum -= shift;
			return shift;
		}

		size_t RemoveAll(const TPredicate<TElementType>& predicate)
		{
			size_t shift = 0;
			for (size_t i = 0; i < m_arrayNum - shift; i++)
			{
				if (predicate(m_pRawPtr[i]))
				{
					i--;
					shift++;
				}

				if (shift)
				{
					DestructElements(i, 1);
					m_pRawPtr[i] = m_pRawPtr[i + shift];
				}
			}

			m_arrayNum -= shift;
			return shift;
		}

		void RemoveAt(size_t index, size_t count = 1)
		{
			DestructElements(index, count);

			size_t i = index;
			while (i + count < m_arrayNum)
			{
				m_pRawPtr[i] = m_pRawPtr[i + count];
			}
			m_arrayNum -= count;
		}

		size_t RemoveAtSwap(size_t index, size_t count = 1)
		{
			DestructElements(index, count);
			for (size_t i = 0; i < count; i++)
			{

				m_pRawPtr[index + i] = m_pRawPtr[m_arrayNum - count + i];
			}
			m_arrayNum -= count;
		}

		size_t RemoveFirst(const TElementType& item)
		{
			size_t res = 0;
			size_t shift = 0;
			for (size_t i = 0; i < m_arrayNum - shift; i++)
			{
				if (m_pRawPtr[i] == item && shift == 0)
				{
					i--;
					shift++;
					res = i;
				}

				if (shift)
				{
					DestructElements(i, 1);
					m_pRawPtr[i] = m_pRawPtr[i + shift];
				}
			}

			m_arrayNum -= 1;
			return res;
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

		void Sort();
		void Sort(const TPredicate<TElementType>& predicate);

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

		TElementType* Data() { return m_pRawPtr; }
		const TElementType* Data() const { return m_pRawPtr; }

	protected:

		__forceinline void ResizeIfNeeded(size_t extraNum)
		{
			if (m_capacity < m_arrayNum + extraNum)
			{
				const size_t optimalSize = Math::UpperPowOf2((uint32_t)(m_capacity + extraNum));
				Reserve(optimalSize);
			}
		}

		const float Factor = 2.0f;

		TAllocator m_allocator{};
		size_t m_arrayNum = 0;
		size_t m_capacity = 0;

		TElementType* m_pRawPtr = nullptr;

		void ConstructElements(size_t index, const TElementType& firstElement, size_t count = 1) requires !IsTriviallyDestructible<TElementType>
		{
			for (size_t i = 0; i < count; i++)
			{
				new (&m_pRawPtr[i + index]) TElementType((&firstElement)[i]);
			}
		}

		void DestructElements(size_t index, size_t count = 1) requires !IsTriviallyDestructible<TElementType>
		{
			for (size_t i = 0; i < m_arrayNum; i++)
			{
				m_pRawPtr[i].~TElementType();
			}
		}

		void DestructElements(size_t index, size_t count = 1) requires IsTriviallyDestructible<TElementType>
		{
			// Do nothing for trivial types
		}
	};
}