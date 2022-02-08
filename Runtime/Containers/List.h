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
	class SAILOR_API TList final
	{
	public:

		class TNode
		{
		public:

			template<typename... TArgs>
			TNode(TArgs&& ... args) noexcept : m_data(std::forward<TArgs>(args) ...), m_pNext(nullptr), m_pPrev(nullptr) {}

			TElementType m_data;
			TNode* m_pNext;
			TNode* m_pPrev;
		};

		template<typename TDataType = TElementType>
		class SAILOR_API TBaseIterator
		{
		public:

			using iterator_category = std::bidirectional_iterator_tag;
			using value_type = TDataType;
			using difference_type = int64_t;
			using pointer = TDataType*;
			using reference = TDataType&;

			TBaseIterator() : m_node(nullptr) {}

			TBaseIterator(const TBaseIterator&) = default;
			TBaseIterator(TBaseIterator&&) = default;

			~TBaseIterator() = default;

			TBaseIterator(TNode* node) : m_node(node) {}

			operator TBaseIterator<const TElementType>() { return TBaseIterator<const TElementType>(m_node); }

			TBaseIterator& operator=(const TBaseIterator& rhs) = default;
			TBaseIterator& operator=(TBaseIterator&& rhs) = default;

			bool operator==(const TBaseIterator& rhs) const { return m_node == rhs.m_node; }
			bool operator!=(const TBaseIterator& rhs) const { return m_node != rhs.m_node; }

			pointer operator->() { return &m_node->m_data; }
			pointer operator->() const { return &m_node->m_data; }

			reference operator*() { return m_node->m_data; }
			reference operator*() const { return m_node->m_data; }

			TBaseIterator& operator++()
			{
				m_node = m_node->m_pNext;
				return *this;
			}

			TBaseIterator& operator--()
			{
				m_node = m_node->m_pPrev;
				return *this;
			}

		protected:

			TNode* m_node;
		};

		using TIterator = TBaseIterator<TElementType>;
		using TConstIterator = TBaseIterator<const TElementType>;

		// Constructors & Destructor
		TList() = default;
		~TList() { Clear(); }

		TList(std::initializer_list<TElementType> initList) requires IsCopyConstructible<TElementType> { AddRange(initList.begin(), initList.size()); }
		TList(const TList& other) requires IsCopyConstructible<TElementType>
		{
			for (auto it : other)
			{
				PushBack(it);
			}
		}

		TList(TList&& other) = default;
		TList& operator=(TList&& other) = default;

		TList& operator=(std::initializer_list<TElementType> initList) { AddRange(initList.begin(), initList.size()); return *this; }

		TList& operator=(const TList& other) requires IsCopyConstructible<TElementType>
		{
			for (auto it : other)
			{
				PushBack(it);
			}

			return *this;
		}

		// Methods

		template<typename... TArgs>
		__forceinline void EmplaceBack(TArgs&& ... args)
		{
			TNode* node = static_cast<TNode*>(m_allocator.Allocate(sizeof(TNode)));
			new (node) TNode(std::forward<TArgs>(args)...);

			if (!m_pFirst)
			{
				m_pFirst = m_pLast = node;
			}
			else
			{
				m_pLast->m_pNext = node;
				node->m_pPrev = m_pLast;
				m_pLast = node;
			}

			m_num++;
		}

		template<typename... TArgs>
		__forceinline void EmplaceFront(TArgs&& ... args)
		{
			TNode* node = static_cast<TNode*>(m_allocator.Allocate(sizeof(TNode)));
			new (node) TNode(std::forward<TArgs>(args)...);

			if (!m_pFirst)
			{
				m_pFirst = m_pLast = node;
			}
			else
			{
				m_pFirst->m_pPrev = node;
				node->m_pNext = m_pFirst;
				m_pFirst = node;
			}

			m_num++;
		}

		void PushBack(TElementType item) { EmplaceBack(item); }
		void PushFront(TElementType item) { EmplaceFront(item); }

		void PopBack() { Remove(m_pLast); }
		void PopFront() { Remove(m_pFirst); }

		bool Contains(const TElementType& item) const
		{
			TNode* current = m_pFirst;

			while (current)
			{
				TNode* next = current->m_pNext;
				if (item == current->m_data)
				{
					return true;
				}
				current = next;
			}

			return false;
		}

		bool ContainsIf(const TPredicate<TElementType>& predicate) const
		{
			TNode* current = m_pFirst;

			while (current)
			{
				TNode* next = current->m_pNext;
				if (predicate(current->m_data))
				{
					return true;
				}
				current = next;
			}

			return false;
		}

		TIterator Find(const TElementType& el)
		{
			TNode* current = m_pFirst;

			while (current)
			{
				TNode* next = current->m_pNext;
				if (current->m_data == el)
				{
					return TIterator(current);
				}
				current = next;
			}
			return end();
		}

		TIterator FindIf(const TPredicate<TElementType>& predicate)
		{
			TNode* current = m_pFirst;

			while (current)
			{
				TNode* next = current->m_pNext;
				if (predicate(current->m_data))
				{
					return TIterator(current);
				}
				current = next;
			}
			return end();
		}

		TConstIterator FindIf(const TPredicate<TElementType>& predicate) const
		{
			TNode* current = m_pFirst;

			while (current)
			{
				TNode* next = current->m_pNext;
				if (predicate(current->m_data))
				{
					return TConstIterator(current);
				}
				current = next;
			}
			return end();
		}

		bool FindIf(TElementType*& out, const TPredicate<TElementType>& predicate)
		{
			TNode* current = m_pFirst;

			while (current)
			{
				TNode* next = current->m_pNext;
				if (predicate(current->m_data))
				{
					out = &current->m_data;
					return true;
				}
				current = next;
			}

			return false;
		}

		bool FindIf(const TElementType*& out, const TPredicate<TElementType>& predicate) const
		{
			TNode* current = m_pFirst;

			while (current)
			{
				TNode* next = current->m_pNext;
				if (predicate(current->m_data))
				{
					out = &current->m_data;
					return true;
				}
				current = next;
			}

			return false;
		}

		__forceinline size_t Num() const { return m_num; }

		bool RemoveFirst(const TElementType& item)
		{
			TNode* current = m_pFirst;

			while (current)
			{
				TNode* next = current->m_pNext;
				if (item == current->m_data)
				{
					Remove(current);
					return true;
				}
				current = next;
			}

			return false;
		}

		bool RemoveLast(const TElementType& item)
		{
			TNode* current = m_pLast;

			while (current)
			{
				TNode* prev = current->m_pPrev;
				if (item == current->m_data)
				{
					Remove(current);
					return true;
				}
				current = prev;
			}

			return false;
		}

		size_t RemoveAll(const TElementType& item)
		{
			return RemoveAll([&](const TElementType& el) { return el == item; });
		}

		size_t RemoveAll(const TPredicate<TElementType>& predicate)
		{
			size_t num = 0;
			TNode* current = m_pFirst;

			while (current)
			{
				TNode* next = current->m_pNext;
				if (predicate(current->m_data))
				{
					Remove(current);
					num++;
				}
				current = next;
			}

			return num;
		}

		void Clear()
		{
			TNode* current = m_pFirst;

			while (current)
			{
				TNode* next = current->m_pNext;

				if constexpr (!IsTriviallyDestructible<TElementType>)
				{
					current->~TNode();
				}

				m_allocator.Free(current);

				current = next;
			}

			m_pLast = nullptr;
			m_pFirst = nullptr;
			m_num = 0;
		}

		__forceinline bool IsEmpty() const { return m_num == 0; }

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

		static void Swap(TList& lhs, TList& rhs)
		{
			std::swap(lhs.m_pFirst, rhs.m_pFirst);
			std::swap(lhs.m_pLast, rhs.m_pLast);
			std::swap(lhs.m_allocator, rhs.m_allocator);
			std::swap(lhs.m_num, rhs.m_num);
		}

		// Support ranged for
		TIterator begin() { return TIterator(m_pFirst); }
		TIterator end() { return TIterator(nullptr); }

		TConstIterator begin() const { return TConstIterator(m_pFirst); }
		TConstIterator end() const { return TConstIterator(nullptr); }

		TIterator First() { return begin(); }
		TConstIterator First() const { return begin(); }

		TIterator Last() { return TIterator(m_pLast); }
		TConstIterator Last() const { return TConstIterator(m_pLast); }

	protected:

		__forceinline void AddRange(const TElementType* first, size_t num) requires IsCopyConstructible<TElementType>
		{
			for (size_t i = 0; i < num; i++)
			{
				Add(first[i]);
			}
		}

		__forceinline void Remove(TNode* item)
		{
			assert(item);

			TNode* next = item->m_pNext;
			TNode* prev = item->m_pPrev;

			if constexpr (!IsTriviallyDestructible<TElementType>)
			{
				item->~TNode();
			}

			m_allocator.Free(item);

			if (next)
			{
				next->m_pPrev = prev;
			}

			if (prev)
			{
				prev->m_pNext = next;
			}

			if (item == m_pFirst)
			{
				m_pFirst = next;
			}

			if (item == m_pLast)
			{
				m_pLast = prev;
			}

			m_num--;
		}

		TNode* m_pFirst = nullptr;
		TNode* m_pLast = nullptr;

		size_t m_num = 0;
		TAllocator m_allocator{};
	};

	SAILOR_API void RunListBenchmark();
}