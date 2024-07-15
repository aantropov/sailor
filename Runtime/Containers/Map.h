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
	class TMap final : public TSet<TPair<TKeyType, size_t>, TAllocator>
	{
	public:

		using Super = Sailor::TSet<TPair<TKeyType, size_t>, TAllocator>;
		using TElementType = Sailor::TPair<TKeyType, size_t>;

		template<typename TDataType, typename TElementIterator>
		class SAILOR_API TBaseIterator
		{
		public:

			using iterator_category = std::bidirectional_iterator_tag;
			using value_type = TDataType;
			using difference_type = int64_t;
			using pointer = TDataType*;
			using reference = TDataType&;

			TBaseIterator() : m_map(nullptr), m_it(nullptr), m_currentBucket(nullptr) {}

			TBaseIterator(const TBaseIterator&) = default;
			TBaseIterator(TBaseIterator&&) = default;

			~TBaseIterator() = default;

			TBaseIterator(const TMap* map, Super::TEntry* bucket, TElementIterator it) : m_currentBucket(bucket), m_it(std::move(it)), m_map(const_cast<TMap*>(map)) {}

			operator TBaseIterator<const TDataType, TElementIterator>() { return TBaseIterator<const TDataType, TElementIterator>(m_map, m_currentBucket, m_it); }

			TBaseIterator& operator=(const TBaseIterator& rhs) = default;
			TBaseIterator& operator=(TBaseIterator&& rhs) = default;

			bool operator==(const TBaseIterator& rhs) const { return m_it == rhs.m_it; }
			bool operator!=(const TBaseIterator& rhs) const { return m_it != rhs.m_it; }

			TPair<TKeyType, TValueType*> operator*() { return TPair<TKeyType, TValueType*>(m_it->m_first, &(*m_map->m_values[m_it->m_second])); }
			TPair<TKeyType, const TValueType*> operator*() const { return TPair<TKeyType, TValueType*>(m_it->m_first, &(*m_map->m_values[m_it->m_second])); }

			TPair<TKeyType, TValueType*> operator->() { return TPair<TKeyType, TValueType*>(m_it->m_first, &(*m_map->m_values[m_it->m_second])); }
			TPair<TKeyType, const TValueType*> operator->() const { return TPair<TKeyType, TValueType*>(m_it->m_first, &(*m_map->m_values[m_it->m_second])); }

			const TKeyType& Key() const { return m_it->m_first; }

			TValueType& Value() { return m_map->m_values[m_it->m_second].value(); }
			const TValueType& Value() const { return m_map->m_values[m_it->m_second].value(); }

			/*
			pointer operator->() { return &*m_it; }
			pointer operator->() const { return &*m_it; }

			reference operator*() { return *m_it; }
			reference operator*() const { return *m_it; }
			*/

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
				if (m_it == m_currentBucket->GetContainer().begin())
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

		protected:

			Super::TEntry* m_currentBucket;
			TElementIterator m_it;
			TMap* m_map;
			friend class TEntry;
		};

		using TIterator = TBaseIterator<TElementType, typename Super::TElementContainer::TIterator>;
		using TConstIterator = TBaseIterator<const TElementType, typename Super::TElementContainer::TConstIterator>;

		using TValueContainer = Sailor::TVector<std::optional<TValueType>, TAllocator>;

		TMap(const uint32_t desiredNumBuckets = 16) : Super(desiredNumBuckets)
		{
			m_values.Reserve(desiredNumBuckets);
		}

		TMap(std::initializer_list<TElementType> initList)
		{
			m_values.Reserve(std::max(Super::m_buckets.Num(), initList.size()));
			for (const auto& el : initList)
			{
				Insert(el);
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

		void Add(const TKeyType& key, const TValueType& value) requires IsCopyConstructible<TValueType>
		{
			Insert(key, value);
		}

		void Add(const TKeyType& key, TValueType&& value) requires IsMoveConstructible<TValueType>
		{
			Insert(key, value);
		}

		void Insert(const TKeyType& key, const TValueType& value) requires IsCopyConstructible<TValueType>
		{
			size_t index = 0;
			if (m_freeList.Num() > 0)
			{
				index = *m_freeList.Last();
				m_freeList.RemoveLast();

				m_values[index] = std::move(std::make_optional(value));
			}
			else
			{
				index = m_values.Emplace(std::make_optional(value));
			}

			Super::Insert(TElementType(key, index));
		}

		void Insert(const TKeyType& key, TValueType&& value) requires IsMoveConstructible<TValueType>
		{
			size_t index = 0;
			if (m_freeList.Num() > 0)
			{
				index = *m_freeList.Last();
				m_freeList.RemoveLast();

				m_values[index] = std::move(std::make_optional(std::move(value)));
			}
			else
			{
				index = m_values.Emplace(std::move(std::make_optional(std::move(value))));
			}

			Super::Insert(TElementType(key, index));
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
			for (const auto& bucket : Super::m_buckets)
			{
				if (bucket && bucket->GetContainer().FindIf([&](const TElementType& el) { return *m_values[el.Second()] == value; }) != -1)
				{
					return true;
				}
			}
			return false;
		}

		TVector<TKeyType> GetKeys() const
		{
			TVector<TKeyType> res(Super::Num());

			for (const auto& pair : *this)
			{
				res.Add(pair.m_first);
			}

			return res;
		}

		TVector<TValueType> GetValues() const
		{
			TVector<TValueType> res(Super::Num());

			for (const auto& value : m_values)
			{
				if (value.has_value())
				{
					res.Add(value.value());

					if (res.Capacity() == res.Num())
					{
						break;
					}
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
