#pragma once
#include <cassert>
#include <memory>
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

	template<typename TElementType, typename TAllocator = Memory::GlobalHeapAllocator>
	class SAILOR_API TVector
	{
	public:

		// Constructors & Destructor
		TVector() = default;
		virtual ~TVector() { Clear(0); }

		TVector(std::initializer_list<TElementType> initList);

		TVector(const TVector& other)
		{
			Reserve(other.m_capacity);
			std::memcpy(m_pRawPtr, other.m_pRawPtr, other.m_arrayNum * sizeof(TElementType));
		}

		TVector(TVector&& other)
		{
			Swap(this, other);
		}

		TVector(const TElementType* rawPtr, size_t count)
		{
			m_capacity = m_arrayNum = count;
			m_pRawPtr = rawPtr;
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
			return std::memcmp(m_pRawPtr, otherArray.m_pRawPtr, m_arrayNum * sizeof(TElementType)) == 0;
		}

		TVector& operator=(std::initializer_list<TElementType> initList);

		TVector& operator=(TVector&& other)
		{
			Swap(this, other);
			return this;
		}

		TVector& operator=(const TVector& other)
		{
			Clear(other.m_capacity);
			std::memcpy(m_pRawPtr, other.m_pRawPtr, other.m_arrayNum * sizeof(TElementType));
			return this;
		}

		// Methods

		template<typename... TArgs>
		size_t Emplace(TArgs&& ... args)
		{
			if (m_capacity <= m_arrayNum)
			{
				const size_t newCapacity = Math::UpperPowOf2(m_capacity);
				Reserve(newCapacity);
			}

			new (&m_pRawPtr[m_arrayNum]) TElementType(std::forward(args));
			return m_arrayNum++;
		}

		size_t Add(const TElementType& item)
		{
			return Emplace(item);
		}

		size_t Add(TElementType&& item)
		{
			if (m_capacity <= m_arrayNum)
			{
				Reserve(m_capacity == 0 ? 1 : m_capacity * 2);
			}

			std::swap(m_pRawPtr[m_arrayNum], item);

			return m_arrayNum++;
		}

		void AddRange(const TVector& other)
		{
			if (m_capacity - m_arrayNum < other.m_arrayNum)
			{
				Reserve(Math::UpperPowOf2(m_arrayNum + other.m_arrayNum));
			}

			std::memcpy(&m_pRawPtr[m_arrayNum], other.m_pRawPtr, other.m_arrayNum * sizeof(TElementType));
			m_arrayNum += other.m_arrayNum;
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

		void AddRange(std::initializer_list<TElementType> initList);

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

		bool ContainsIf(const std::function<bool>& predicate) const
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

		size_t FindIf(const std::function<bool>& predicate) const
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

		size_t Insert(const TElementType& item, size_t index);
		size_t Insert(TElementType&& item, size_t index);
		size_t Insert(const TVector& vector, size_t index);
		size_t Insert(TVector&& vector, size_t index);
		size_t Insert(std::initializer_list<TElementType> initList, size_t index);

		bool IsValidIndex(size_t index) const { return 0 < index && index < m_arrayNum; }
		size_t Num() const { return m_arrayNum; }

		size_t Remove(const TElementType& item);
		size_t RemoveAll(const std::function<bool>& predicate);
		size_t RemoveAt(size_t index, size_t count = 1);
		size_t RemoveAtSwap(size_t index, size_t count = 1);
		size_t RemoveFirst(const TElementType& item);
		size_t RemoveFirstSwap(const TElementType& item);

		void Clear(size_t newCapacity = 0)
		{
			m_allocator.Free(m_pRawPtr);
			m_arrayNum = 0;
			m_pRawPtr = nullptr;
			m_capacity = 0;

			if (newCapacity)
			{
				Reserve(newCapacity);
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
		void Sort(const std::function<bool>& predicate);

		void Swap(TVector& lhs, TVector& rhs) const
		{
			std::swap(lhs.m_pRawPtr, rhs.m_pRawPtr);
			std::swap(lhs.m_allocator, rhs.m_allocator);
			std::swap(lhs.m_arrayNum, rhs.m_arrayNum);
			std::swap(lhs.m_capacity, rhs.m_capacity);
		}

		// For each

		TIterator<TElementType> begin() { return TIterator<TElementType>(m_pRawPtr); }
		TIterator<TElementType> end() { return TIterator<TElementType>(m_pRawPtr + m_arrayNum); }

	protected:

		const float Factor = 2.0f;

		TAllocator m_allocator{};
		size_t m_arrayNum = 0;
		size_t m_capacity = 0;

		TElementType* m_pRawPtr = nullptr;
	};
}