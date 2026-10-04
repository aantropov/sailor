#pragma once
#include <cassert>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include "Core/Defines.h"
#include "Memory/LockFreeHeapAllocator.h"
#include "Containers/Vector.h"
#include "Containers/Set.h"
#include "Containers/Pair.h"

namespace Sailor
{
	template<typename TKeyType, typename TValueType, typename TAllocator = Memory::DefaultGlobalAllocator>
	class TMap final : private TSet<TPair<TKeyType, size_t>, TAllocator>
	{
		using Super = Sailor::TSet<TPair<TKeyType, size_t>, TAllocator>;
		using TElementType = Sailor::TPair<TKeyType, size_t>;

	public:

		using Super::IsEmpty;
		using Super::Num;

		template<typename TDataType, typename TElementIterator>
		class SAILOR_API TBaseIterator
		{
		public:

			using TMapType = std::conditional_t<std::is_const_v<TDataType>, const TMap, TMap>;
			using TValue = std::conditional_t<std::is_const_v<TDataType>, const TValueType, TValueType>;
			using iterator_category = std::bidirectional_iterator_tag;
			using value_type = TPair<TKeyType, TValue*>;
			using difference_type = int64_t;
			using reference = value_type;

			struct TArrowProxy
			{
				value_type m_value;
				const value_type* operator->() const { return &m_value; }
			};
			using pointer = TArrowProxy;

			TBaseIterator() : m_map(nullptr), m_it(nullptr), m_currentBucket(nullptr) {}

			TBaseIterator(const TBaseIterator&) = default;
			TBaseIterator(TBaseIterator&&) = default;

			~TBaseIterator() = default;

			TBaseIterator(TMapType* map, Super::TEntry* bucket, TElementIterator it) : m_currentBucket(bucket), m_it(std::move(it)), m_map(map) {}

			operator TBaseIterator<const TElementType, typename Super::TElementContainer::TConstIterator>() const
				requires (!std::is_const_v<TDataType>)
			{
				return { m_map, m_currentBucket, m_it };
			}

			TBaseIterator& operator=(const TBaseIterator& rhs) = default;
			TBaseIterator& operator=(TBaseIterator&& rhs) = default;

			bool operator==(const TBaseIterator& rhs) const { return m_it == rhs.m_it; }
			bool operator!=(const TBaseIterator& rhs) const { return m_it != rhs.m_it; }

			value_type operator*() const { return { m_it->m_first, &Value() }; }
			pointer operator->() const { return { operator*() }; }

			const TKeyType& Key() const { return m_it->m_first; }

			TValue& Value() const { return m_map->m_values[m_it->m_second].value(); }

			TBaseIterator& operator++()
			{
				++m_it;

				if (m_it == m_currentBucket->GetContainer().end())
				{
					if (m_currentBucket->m_next)
					{
						m_currentBucket = m_currentBucket->m_next;
						m_it = m_currentBucket->GetContainer().begin();
					}
				}

				return *this;
			}

			TBaseIterator& operator--()
			{
				if (m_it == m_currentBucket->GetContainer().end())
				{
					m_it = m_currentBucket->GetContainer().Last();
				}
				else if (m_it == m_currentBucket->GetContainer().begin())
				{
					if (m_currentBucket->m_prev)
					{
						m_currentBucket = m_currentBucket->m_prev;
						m_it = m_currentBucket->GetContainer().Last();
					}
				}
				else
				{
					--m_it;
				}

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

			Super::TEntry* m_currentBucket;
			TElementIterator m_it;
			TMapType* m_map;
			friend class TEntry;
		};

		using TIterator = TBaseIterator<TElementType, typename Super::TElementContainer::TIterator>;
		using TConstIterator = TBaseIterator<const TElementType, typename Super::TElementContainer::TConstIterator>;

		using TValueContainer = Sailor::TVector<std::optional<TValueType>, TAllocator>;

		TMap(const uint32_t desiredNumBuckets = 16) : Super(desiredNumBuckets)
		{
			m_values.Reserve(desiredNumBuckets);
		}

		TMap(std::initializer_list<TPair<TKeyType, TValueType>> initList)
		{
			m_values.Reserve(std::max(Super::m_buckets.Num(), initList.size()));
			for (const auto& el : initList)
			{
				Insert(el.First(), el.Second());
			}
		}

		TMap(const TMap& rhs)
		{
			Clear((uint32_t)rhs.m_buckets.Num());
			for (const auto& el : rhs)
			{
				Insert(el.m_first, *el.m_second);
			}
		}

		TMap& operator=(const TMap& rhs)
		{
			if (this == &rhs)
			{
				return *this;
			}
			Clear((uint32_t)rhs.m_buckets.Num());
			for (const auto& el : rhs)
			{
				Insert(el.m_first, *el.m_second);
			}
			return *this;
		}

		TMap(TMap&&) = default;
		TMap& operator=(TMap&&) noexcept = default;

		~TMap() = default;

		static void Swap(TMap& lhs, TMap& rhs)
		{
			Super::Swap(lhs, rhs);
			TValueContainer::Swap(lhs.m_values, rhs.m_values);
			TVector<size_t, TAllocator>::Swap(lhs.m_freeList, rhs.m_freeList);
		}

		bool operator==(const TMap& rhs) const
		{
			if (Num() != rhs.Num())
			{
				return false;
			}

			for (const auto& pair : *this)
			{
				const auto other = rhs.Find(pair.First());
				if (other == rhs.end() || !Sailor::Equals(*pair.Second(), other.Value()))
				{
					return false;
				}
			}
			return true;
		}

		void Add(const TKeyType& key, const TValueType& value) requires IsCopyConstructible<TValueType>
		{
			Insert(key, value);
		}

		void Add(const TKeyType& key, TValueType&& value) requires IsMoveConstructible<TValueType>
		{
			Insert(key, std::move(value));
		}

		bool Insert(const TKeyType& key, const TValueType& value) requires IsCopyConstructible<TValueType>
		{
			if (ContainsKey(key))
			{
				return false;
			}

			size_t index = 0;
			if (m_freeList.Num() > 0)
			{
				index = *m_freeList.Last();
				m_freeList.RemoveLast();

				m_values[index].emplace(value);
			}
			else
			{
				index = m_values.Emplace(std::in_place, value);
			}

			Super::Insert(TElementType(key, index));
			return true;
		}

		bool Insert(const TKeyType& key, TValueType&& value) requires IsMoveConstructible<TValueType>
		{
			if (ContainsKey(key))
			{
				return false;
			}

			size_t index = 0;
			if (m_freeList.Num() > 0)
			{
				index = *m_freeList.Last();
				m_freeList.RemoveLast();

				m_values[index].emplace(std::move(value));
			}
			else
			{
				index = m_values.Emplace(std::in_place, std::move(value));
			}

			Super::Insert(TElementType(key, index));
			return true;
		}

		bool Remove(const TKeyType& key)
		{
			const auto& hash = Sailor::GetHash(key);
			auto& element = Super::m_buckets[hash % Super::m_buckets.Num()];

			if (element)
			{
				auto& container = element->GetContainer();
				if (container.RemoveAll([&](const TElementType& el)
					{
						if (el.First() == key)
						{
							m_values[el.m_second].reset();
							m_freeList.Add(el.m_second);

							return true;
						}
						return false;

					}))
				{
					if (container.Num() == 0)
					{
						if (element.GetRawPtr() == Super::m_last)
						{
							Super::m_last = element->m_prev;
						}

						if (element->m_next)
						{
							element->m_next->m_prev = element->m_prev;
						}

						if (element->m_prev)
						{
							element->m_prev->m_next = element->m_next;
						}

						if (Super::m_last == element.GetRawPtr())
						{
							Super::m_last = Super::m_last->m_prev;
						}

						if (Super::m_first == element.GetRawPtr())
						{
							Super::m_first = Super::m_first->m_next;
						}

						element.Clear();
					}

					Super::m_num--;
					return true;
				}
				return false;
			}
			return false;
		}

		TValueType& UpdateKey(const TKeyType& key)
		{
			auto& pair = GetOrAdd(key);
			pair.m_first = key;
			return *m_values[pair.m_second];
		}

		TValueType& operator[] (const TKeyType& key)
		{
			return *m_values[GetOrAdd(key).m_second];
		}

		void Clear(uint32_t desiredBucketsNum = 8)
		{
			Super::Clear(desiredBucketsNum);
			m_values.Clear();
			m_freeList.Clear();
		}

		// TODO: rethink the approach for const operator []
		const TValueType& operator[] (const TKeyType& key) const
		{
			TValueType const* out = nullptr;
			Find(key, out);
			return *out;
		}

		bool Find(const TKeyType& key, TValueType*& out)
		{
			auto it = Find(key);
			if (it != end())
			{
				out = &it.Value();
				return true;
			}
			return false;
		}

		bool Find(const TKeyType& key, TValueType const*& out) const
		{
			auto it = Find(key);
			if (it != end())
			{
				out = &it.Value();
				return true;
			}
			return false;
		}

		TIterator Find(const TKeyType& key)
		{
			const auto& hash = Sailor::GetHash(key);
			auto& element = Super::m_buckets[hash % Super::m_buckets.Num()];

			if (element && element->LikelyContains(hash))
			{
				auto& container = element->GetContainer();
				typename Super::TElementContainer::TIterator it = container.FindIf([&](const TElementType& el) { return el.First() == key; });
				if (it != container.end())
				{
					return TIterator(this, element.GetRawPtr(), it);
				}
			}

			return end();
		}

		TConstIterator Find(const TKeyType& key) const
		{
			const auto& hash = Sailor::GetHash(key);
			auto& element = Super::m_buckets[hash % Super::m_buckets.Num()];

			if (element && element->LikelyContains(hash))
			{
				auto& container = element->GetContainer();
				typename Super::TElementContainer::TConstIterator it = container.FindIf([&](const TElementType& el) { return el.First() == key; });
				if (it != container.end())
				{
					return TConstIterator(this, element.GetRawPtr(), it);
				}
			}

			return end();
		}

		bool ContainsKey(const TKeyType& key) const
		{
			return Find(key) != end();
		}

		bool ContainsValue(const TValueType& value) const
		{
			for (const auto& stored : m_values)
			{
				if (stored && *stored == value)
				{
					return true;
				}
			}
			return false;
		}

		TVector<TKeyType> GetKeys() const
		{
			TVector<TKeyType> res;
			res.Reserve(Super::Num());

			for (const auto& pair : *this)
			{
				res.Add(pair.m_first);
			}

			return res;
		}

		TVector<TValueType> GetValues() const
		{
			TVector<TValueType> res;
			res.Reserve(Super::Num());

			for (const auto& value : m_values)
			{
				if (value.has_value())
				{
					res.Add(value.value());
				}
			}

			return res;
		}

		// Support ranged for
		TIterator begin() { return TIterator(this, Super::m_first, Super::m_first ? Super::m_first->GetContainer().begin() : nullptr); }
		TIterator end() { return TIterator(this, Super::m_last, nullptr); }

		TConstIterator begin() const { return TConstIterator(this, Super::m_first, Super::m_first ? Super::m_first->GetContainer().begin() : nullptr); }
		TConstIterator end() const { return TConstIterator(this, Super::m_last, nullptr); }

	protected:

		TValueContainer m_values;
		TVector<size_t, TAllocator> m_freeList;

		TElementType& GetOrAdd(const TKeyType& key)
		{
			const auto& hash = Sailor::GetHash(key);
			{
				const size_t index = hash % Super::m_buckets.Num();
				auto& element = Super::m_buckets[index];

				if (element)
				{
					auto& container = element->GetContainer();

					TElementType* out;
					if (container.FindIf(out, [&](const TElementType& element) { return element.First() == key; }))
					{
						return *out;
					}
				}
			}

			// TODO: rethink the approach when default constructor is missed
			Insert(key, std::move(TValueType()));

			const size_t index = hash % Super::m_buckets.Num();
			auto& element = Super::m_buckets[index];

			return *element->GetContainer().Last();
		}

		friend TIterator;
		friend TConstIterator;
	};

	SAILOR_API void RunMapBenchmark();
}
