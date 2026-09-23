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
#include "Containers/Vector.h"

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
			TNode* m_pNext{};
			TNode* m_pPrev{};
		};

		template<typename TDataType = TElementType>
		class SAILOR_API TBaseIterator
		{
		public:

			using iterator_category = std::bidirectional_iterator_tag;
			using value_type = std::remove_const_t<TDataType>;
			using difference_type = int64_t;
			using pointer = TDataType*;
			using reference = TDataType&;

			TBaseIterator() : m_node(nullptr) {}

			TBaseIterator(const TBaseIterator&) = default;
			TBaseIterator(TBaseIterator&&) = default;

			~TBaseIterator() = default;

			TBaseIterator(TNode* node, const TList* owner = nullptr) : m_node(node), m_owner(owner) {}

			operator TBaseIterator<const TElementType>() const requires (!std::is_const_v<TDataType>)
			{
				return { m_node, m_owner };
			}

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
				m_node = m_node ? m_node->m_pPrev : m_owner->m_pLast;
				return *this;
			}

			TBaseIterator operator++(int)
			{
				auto previous = *this;
				++(*this);
				return previous;
			}

			TBaseIterator operator--(int)
			{
				auto previous = *this;
				--(*this);
				return previous;
			}

		protected:

			TNode* m_node;
			const TList* m_owner = nullptr;
		};

		using TIterator = TBaseIterator<TElementType>;
		using TConstIterator = TBaseIterator<const TElementType>;

		// Constructors & Destructor
		TList() = default;
		~TList() { Clear(); }

		TList(std::initializer_list<TElementType> initList) requires IsCopyConstructible<TElementType> { AddRange(initList.begin(), initList.size()); }
		TList(const TList& other) requires IsCopyConstructible<TElementType>
		{
			for (const auto& it : other)
			{
				EmplaceBack(it);
			}
		}

		TList(TList&& other) noexcept requires IsMoveConstructible<TAllocator>
		{
			Swap(*this, other);
		}

		TList& operator=(TList&& other) noexcept requires IsMoveConstructible<TAllocator>
		{
			if (this != &other)
			{
				Clear();
				Swap(*this, other);
			}
			return *this;
		}

		TList& operator=(std::initializer_list<TElementType> initList)
		{
			Clear();
			AddRange(initList.begin(), initList.size());
			return *this;
		}

		TList& operator=(const TList& other) requires IsCopyConstructible<TElementType>
		{
			if (this == &other)
			{
				return *this;
			}
			Clear();
			for (const auto& it : other)
			{
				EmplaceBack(it);
			}

			return *this;
		}

		// Methods

		template<typename... TArgs>
		__forceinline void EmplaceBack(TArgs&& ... args)
		{
			TNode* node = static_cast<TNode*>(m_allocator.Allocate(sizeof(TNode), alignof(TNode)));
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
			TNode* node = static_cast<TNode*>(m_allocator.Allocate(sizeof(TNode), alignof(TNode)));
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

		void PushBack(TElementType item) { EmplaceBack(std::move_if_noexcept(item)); }
		void PushFront(TElementType item) { EmplaceFront(std::move_if_noexcept(item)); }

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
					return TIterator(current, this);
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
					return TIterator(current, this);
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
					return TConstIterator(current, this);
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
			Sort([](const TElementType& lhs, const TElementType& rhs) { return lhs < rhs; });
		}

		void Sort(const TCompare<TElementType>& compare)
		{
			if (m_num < 2)
			{
				return;
			}
			TVector<TNode*> nodes;
			nodes.Reserve(m_num);
			for (TNode* node = m_pFirst; node; node = node->m_pNext)
			{
				nodes.Add(node);
			}
			nodes.Sort([&](const TNode* lhs, const TNode* rhs) { return compare(lhs->m_data, rhs->m_data); });
			for (size_t i = 0; i < nodes.Num(); ++i)
			{
				nodes[i]->m_pPrev = i > 0 ? nodes[i - 1] : nullptr;
				nodes[i]->m_pNext = i + 1 < nodes.Num() ? nodes[i + 1] : nullptr;
			}
			m_pFirst = *nodes.First();
			m_pLast = *nodes.Last();
		}

		static void Swap(TList& lhs, TList& rhs)
		{
			std::swap(lhs.m_pFirst, rhs.m_pFirst);
			std::swap(lhs.m_pLast, rhs.m_pLast);
			std::swap(lhs.m_allocator, rhs.m_allocator);
			std::swap(lhs.m_num, rhs.m_num);
		}

		// Support ranged for
		TIterator begin() { return TIterator(m_pFirst, this); }
		TIterator end() { return TIterator(nullptr, this); }

		TConstIterator begin() const { return TConstIterator(m_pFirst, this); }
		TConstIterator end() const { return TConstIterator(nullptr, this); }

		TIterator First() { return begin(); }
		TConstIterator First() const { return begin(); }

		TIterator Last() { return TIterator(m_pLast, this); }
		TConstIterator Last() const { return TConstIterator(m_pLast, this); }

		template<typename TAllocator1>
		__forceinline bool operator==(const TList<TElementType, TAllocator1>& rhs) const
		{
			if (m_num != rhs.Num())
			{
				return false;
			}

			auto rhsIt = rhs.begin();
			for (const auto& element : *this)
			{
				if (element != *rhsIt)
				{
					return false;
				}
				++rhsIt;
			}

			return true;
		}

	protected:

		__forceinline void AddRange(const TElementType* first, size_t num) requires IsCopyConstructible<TElementType>
		{
			for (size_t i = 0; i < num; i++)
			{
				EmplaceBack(first[i]);
			}
		}

		__forceinline void Remove(TNode* item)
		{
			check(item);

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
