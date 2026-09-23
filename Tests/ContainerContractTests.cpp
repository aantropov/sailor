#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Memory/Memory.h"
#include "Containers/Vector.h"
#include "Containers/List.h"
#include "Containers/Map.h"
#include "Containers/Octree.h"
#include "Containers/Octree2.h"

using namespace Sailor;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	struct Lifetime
	{
		explicit Lifetime(int value = 0, int tag = 0) : m_value(value), m_tag(tag)
		{
			s_invalidLifetime |= !s_live.insert(this).second;
		}

		Lifetime(const Lifetime& other) : m_value(other.m_value), m_tag(other.m_tag)
		{
			s_invalidLifetime |= !s_live.contains(&other) || !s_live.insert(this).second;
		}

		Lifetime(Lifetime&& other) noexcept : m_value(other.m_value), m_tag(other.m_tag)
		{
			s_invalidLifetime |= !s_live.contains(&other) || !s_live.insert(this).second;
			other.m_value = -1;
		}

		Lifetime& operator=(const Lifetime&) = delete;
		Lifetime& operator=(Lifetime&&) = delete;

		~Lifetime()
		{
			s_invalidLifetime |= s_live.erase(this) != 1;
		}

		bool operator==(const Lifetime& other) const { return m_value == other.m_value; }
		bool operator<(const Lifetime& other) const { return m_value < other.m_value; }

		int m_value;
		int m_tag;
		static inline std::unordered_set<const Lifetime*> s_live;
		static inline bool s_invalidLifetime = false;
	};

	struct MoveOnly : Lifetime
	{
		using Lifetime::Lifetime;
		MoveOnly(const MoveOnly&) = delete;
		MoveOnly(MoveOnly&&) noexcept = default;
	};

	struct CopyOnly : Lifetime
	{
		using Lifetime::Lifetime;
		CopyOnly(const CopyOnly&) = default;
		CopyOnly(CopyOnly&&) = delete;
	};

	struct ConstructOnly
	{
		explicit ConstructOnly(int value) : m_value(value) {}
		ConstructOnly(const ConstructOnly&) = delete;
		ConstructOnly(ConstructOnly&&) noexcept = default;
		ConstructOnly& operator=(const ConstructOnly&) = delete;
		ConstructOnly& operator=(ConstructOnly&&) = delete;
		int m_value;
	};

	struct alignas(64) AlignedValue
	{
		explicit AlignedValue(int value = 0) : m_value(value) {}
		int m_value;
	};

	static_assert(std::random_access_iterator<TVector<int>::TIterator>);
	static_assert(std::bidirectional_iterator<TList<int>::TIterator>);
	static_assert(std::bidirectional_iterator<TList<int>::TConstIterator>);
	static_assert(std::is_same_v<decltype(std::declval<TMap<int, int>::TConstIterator>().Value()), const int&>);
	static_assert(std::is_same_v<decltype((*std::declval<TMap<int, int>::TConstIterator>()).Second()), const int* const&>);
	static_assert(std::is_convertible_v<TMap<int, int>::TIterator, TMap<int, int>::TConstIterator>);
	static_assert(!std::is_convertible_v<TMap<int, int>*, TSet<TPair<int, size_t>>*>);
	static_assert(!std::is_copy_assignable_v<TPair<ConstructOnly, ConstructOnly>>);
	static_assert(!std::is_move_assignable_v<TPair<ConstructOnly, ConstructOnly>>);

	void CheckLifetime(size_t expected)
	{
		Require(!Lifetime::s_invalidLifetime, "object was constructed over, copied from dead storage, or destroyed twice");
		Require(Lifetime::s_live.size() == expected, "container lost a live object or retained a removed object");
	}

	template<typename T>
	int ValueOf(const T& value)
	{
		if constexpr (std::is_same_v<T, int>) return value;
		else return value.m_value;
	}

	template<typename T>
	void TestVectorEraseRanges()
	{
		for (bool swapRemoval : { false, true })
		{
			for (size_t size = 0; size <= 8; ++size)
			{
				for (size_t index = 0; index <= size; ++index)
				{
					for (size_t count = 0; count <= size - index; ++count)
					{
						{
							TVector<T> values;
							std::vector<int> expected;
							for (size_t i = 0; i < size; ++i)
							{
								values.Emplace(static_cast<int>(i));
								expected.push_back(static_cast<int>(i));
							}
							if (swapRemoval)
							{
								const size_t moved = std::min(count, size - index - count);
								for (size_t i = 0; i < moved; ++i) expected[index + i] = expected[size - moved + i];
								expected.resize(size - count);
								values.RemoveAtSwap(index, count);
							}
							else
							{
								expected.erase(expected.begin() + index, expected.begin() + index + count);
								values.RemoveAt(index, count);
							}
							Require(values.Num() == expected.size(), "erase must remove exactly count elements");
							for (size_t i = 0; i < expected.size(); ++i)
							{
								Require(ValueOf(values[i]) == expected[i], "erase must preserve the surviving values");
							}
							CheckLifetime(std::is_same_v<T, int> ? 0 : values.Num());
						}
						CheckLifetime(0);
					}
				}
			}
		}
	}

	void TestVectorConstructionAndAssignment()
	{
		const TVector<int> empty(static_cast<const int*>(nullptr), 0);
		TVector<int> emptyCopy(empty);
		Require(emptyCopy.IsEmpty(), "copying an empty vector must not dereference its storage");
		TVector<int> defaults(3);
		Require(defaults.Num() == 3 && defaults[2] == 0, "count constructor must still create default elements");
		{
			CopyOnly prototype(7);
			TVector<CopyOnly> filled(3, prototype);
			Require(filled.Num() == 3, "fill constructor must create N elements, not 2N");
			for (const auto& element : filled) Require(element.m_value == 7, "fill constructor must copy its value");
			CheckLifetime(4);
			TVector<CopyOnly> copied(filled);
			auto& same = copied;
			copied = same;
			Require(copied.Num() == 3 && copied[0].m_value == 7, "vector self-assignment must preserve data");
			TVector<CopyOnly> replacement;
			replacement.Emplace(99);
			replacement = filled;
			CheckLifetime(10);
			replacement.Reserve(64);
			CheckLifetime(10);
			Require(replacement.Num() == 3 && replacement[2].m_value == 7, "reserve must relocate live elements only");
		}
		CheckLifetime(0);
	}

	void TestContainerAlignment()
	{
		TVector<AlignedValue> vector;
		for (int i = 0; i < 20; ++i) vector.Emplace(i);
		vector.Reserve(64);
		for (size_t i = 0; i < vector.Num(); ++i)
		{
			Require(reinterpret_cast<uintptr_t>(&vector[i]) % alignof(AlignedValue) == 0,
				"vector allocation and growth must preserve element alignment");
			Require(vector[i].m_value == static_cast<int>(i), "aligned vector growth must preserve values");
		}
		TList<AlignedValue> list;
		for (int i = 0; i < 20; ++i)
		{
			if (i % 2) list.EmplaceFront(i);
			else list.EmplaceBack(i);
		}
		for (const auto& value : list)
		{
			Require(reinterpret_cast<uintptr_t>(&value) % alignof(AlignedValue) == 0,
				"list nodes must preserve the stored element alignment");
		}
	}

	template<typename T>
	void TestVectorInsertionAndRemoveFirst()
	{
		{
			TVector<T> values;
			values.Emplace(1);
			values.Emplace(2);
			values.Emplace(3);
			const T inserted[]{ T(10), T(11), T(12) };
			values.Insert(inserted, 3, 1);
			const int expected[]{ 1, 10, 11, 12, 2, 3 };
			for (size_t i = 0; i < values.Num(); ++i)
			{
				Require(ValueOf(values[i]) == expected[i], "insertion must preserve the overlapping tail");
			}
			values.RemoveFirst(values[4]);
			Require(values.Num() == 5 && ValueOf(values[4]) == 3, "RemoveFirst must shift and destroy a one-element tail");
			values.RemoveFirst(values[4]);
			Require(values.Num() == 4, "RemoveFirst must remove the last element exactly once");
			CheckLifetime(std::is_same_v<T, int> ? 0 : values.Num() + 3);
		}
		CheckLifetime(0);
	}

	void TestListOwnershipAndSorting()
	{
		TList<int> initialized{ 3, 1, 2 };
		initialized = { 8, 9 };
		Require(initialized.Num() == 2 && *initialized.First() == 8, "initializer assignment must replace list contents");
		{
			TList<Lifetime> original;
			original.EmplaceBack(2, 0);
			original.EmplaceBack(1, 1);
			original.EmplaceBack(2, 2);
			const Lifetime* firstAddress = &*original.First();
			original.Sort();
			const int tags[]{ 1, 0, 2 };
			size_t index = 0;
			for (const auto& element : original) Require(element.m_tag == tags[index++], "list sort must be stable");
			Require(&*++original.begin() == firstAddress, "list sort must relink nodes without moving their values");
			CheckLifetime(3);
			TList<Lifetime> copied(original);
			copied.PopFront();
			Require(original.Num() == 3 && copied.Num() == 2, "list copy must own independent nodes");
			copied = original;
			Require(copied.Num() == 3, "list copy assignment must replace, not append");
			auto& same = copied;
			copied = same;
			Require(copied.Num() == 3, "list self-assignment must preserve nodes");
			TList<Lifetime> moved(std::move(copied));
			Require(copied.IsEmpty() && moved.Num() == 3, "list move constructor must transfer node ownership");
			TList<Lifetime> replacement;
			replacement.EmplaceBack(42);
			replacement = std::move(moved);
			Require(moved.IsEmpty() && replacement.Num() == 3, "list move assignment must replace ownership");
			auto& sameMoved = replacement;
			replacement = std::move(sameMoved);
			Require(replacement.Num() == 3, "list self-move must preserve ownership");
			CheckLifetime(6);
		}
		CheckLifetime(0);
		{
			TList<MoveOnly> values;
			values.PushBack(MoveOnly(5));
			values.PushFront(MoveOnly(8));
			values.Sort([](const MoveOnly& lhs, const MoveOnly& rhs) { return lhs.m_value > rhs.m_value; });
			Require(values.First()->m_value == 8 && values.Last()->m_value == 5, "list must support move-only nonassignable values");
			CheckLifetime(2);
		}
		CheckLifetime(0);
	}

	void TestIteratorInterfaces()
	{
		TVector<int> vector{ 4, 5, 6 };
		const auto begin = vector.begin();
		Require(*(2 + begin) == 6, "integer plus const iterator must be supported");
		TList<int> list{ 4, 5, 6 };
		auto it = list.begin();
		Require(*it++ == 4 && *it == 5, "list post-increment must return the old iterator");
		Require(*it-- == 5 && *it == 4, "list post-decrement must return the old iterator");
		auto end = list.end();
		list.PushBack(7);
		Require(*--end == 7, "decrementing end must use the current tail");
		const TList<int>& constList = list;
		int expected = 7;
		for (auto reverse = std::make_reverse_iterator(constList.end());
			reverse != std::make_reverse_iterator(constList.begin()); ++reverse)
		{
			Require(*reverse == expected--, "const list reverse traversal must preserve order");
		}
	}

	void TestMapExtractionAndConstIteration()
	{
		TMap<int, int> map;
		Require(map.GetKeys().IsEmpty() && map.GetValues().IsEmpty(), "empty map extraction must be empty");
		map.Add(0, 100);
		Require(map.GetKeys().Num() == 1 && map.GetValues()[0] == 100, "one real zero key must not produce a default prefix");
		for (int key = 1; key < 10; ++key) map.Add(key, 100 + key);
		map.Remove(1);
		map.Remove(6);
		map.Remove(8);
		map.Add(17, 117);
		map.Add(18, 118);
		auto keys = map.GetKeys();
		auto values = map.GetValues();
		keys.Sort();
		values.Sort();
		const int expected[]{ 0, 2, 3, 4, 5, 7, 9, 17, 18 };
		Require(keys.Num() == 9 && values.Num() == 9, "map extraction must include each occupied slot exactly once");
		for (size_t i = 0; i < keys.Num(); ++i)
		{
			Require(keys[i] == expected[i] && values[i] == 100 + expected[i], "holes must not truncate or add map entries");
		}
		Require(map.ContainsValue(117) && !map.ContainsValue(101), "ContainsValue must ignore removed slots");
		auto& same = map;
		map = same;
		Require(map.Num() == 9 && map[17] == 117, "map self-assignment must preserve entries");
		const TMap<int, int>& constMap = map;
		auto constIt = constMap.Find(17);
		Require(constIt.Value() == 117 && *constIt->Second() == 117, "const iterator dereference and arrow must read the value");
		auto mutableIt = map.Find(17);
		mutableIt.Value() = 217;
		*mutableIt->Second() = 317;
		TMap<int, int>::TConstIterator converted = mutableIt;
		Require(converted.Value() == 317, "mutable iterator must convert to a read-only iterator");
		size_t count = 0;
		for (auto reverse = map.end(); reverse != map.begin();)
		{
			--reverse;
			Require(map.ContainsKey(reverse.Key()), "map reverse traversal must visit live entries");
			++count;
		}
		Require(count == map.Num(), "map decrement from end must traverse every occupied bucket");
		TMap<int, std::string> initialized{ { 0, "zero" }, { 2, "two" } };
		Require(initialized.Num() == 2 && initialized[2] == "two", "map initializer list must contain values, not internal indices");
		{
			TMap<int, MoveOnly> owned;
			owned.Add(1, MoveOnly(10));
			owned.Add(2, MoveOnly(20));
			owned.Remove(1);
			owned.Add(3, MoveOnly(30));
			Require(owned.Num() == 2 && owned.Find(3).Value().m_value == 30, "map insertion must construct move-only values in reused slots");
			CheckLifetime(2);
		}
		CheckLifetime(0);
	}

	void TestMapValueEqualityAndSwap()
	{
		TMap<int, int> first{ { 1, 10 }, { 2, 20 } };
		TMap<int, int> reordered{ { 2, 20 }, { 1, 10 } };
		Require(first == reordered, "map equality must compare values, not insertion slot indices");
		Require(first == first, "map equality must support self-comparison");
		reordered[1] = 99;
		Require(first != reordered, "matching keys with different values must not compare equal");
		reordered[1] = 10;
		reordered.Remove(2);
		reordered.Add(3, 20);
		Require(first != reordered, "equal size and values must not hide a different key");

		first.Add(7, 70);
		first.Remove(2);
		TMap<int, int> second{ { 4, 40 } };
		TMap<int, int>::Swap(first, second);
		Require(first.Num() == 1 && first[4] == 40 && second.Num() == 2 &&
			second[1] == 10 && second[7] == 70,
			"map swap must exchange values and keys together");
		first.Add(5, 50);
		second.Add(8, 80);
		Require(first.GetValues().Num() == 2 && second.GetValues().Num() == 3 && second[8] == 80,
			"map swap must transfer reusable value slots with their values");
		const TMap<int, int> beforeSelfSwap(second);
		TMap<int, int>::Swap(second, second);
		Require(second == beforeSelfSwap, "map self-swap must preserve all values");
	}

	void TestPairConstruction()
	{
		TPair<ConstructOnly, ConstructOnly> original(ConstructOnly(3), ConstructOnly(7));
		TPair<ConstructOnly, ConstructOnly> moved(std::move(original));
		Require(moved.First().m_value == 3 && moved.Second().m_value == 7,
			"pair move construction must not require default construction or assignment");
		{
			CopyOnly copy(13);
			TPair<ConstructOnly, CopyOnly> mixed(ConstructOnly(11), copy);
			TPair<ConstructOnly, CopyOnly> movedMixed(std::move(mixed));
			Require(movedMixed.First().m_value == 11 && movedMixed.Second().m_value == 13,
				"pair move construction must copy a copy-only member");
			CheckLifetime(3);
		}
		CheckLifetime(0);
	}

	void TestSetAndMapMoveContracts()
	{
		TSet<int> source{ 1, 2 };
		TSet<int> target{ 5, 6, 7 };
		target = std::move(source);
		Require(target.Num() == 2 && target.Contains(1) && target.Contains(2), "set move assignment must transfer buckets and metadata together");
		Require(source.Num() == 3 && source.Contains(5) && source.Contains(7), "set swap assignment must leave a coherent source");
		source.Insert(8);
		Require(source.Num() == 4 && source.Contains(8), "set move source must remain reusable");
		TSet<int> moved(std::move(target));
		Require(target.IsEmpty() && moved.Num() == 2 && moved.Contains(2), "set move construction must leave an empty source");
		target.Insert(9);
		Require(target.Num() == 1 && target.Contains(9), "set move-construction source must retain insertable buckets");
		auto& sameSet = moved;
		moved = sameSet;
		Require(moved.Num() == 2 && moved.Contains(1), "set self-copy must preserve elements");
		TSet<int> empty(std::initializer_list<int>{});
		empty.Insert(11);
		Require(empty.Contains(11), "empty initializer-list set must accept insertion");
		empty.Clear(0);
		empty.Insert(12);
		Require(empty.Num() == 1 && empty.Contains(12), "Clear(0) must retain an insertable set");

		TMap<int, int> mapSource;
		mapSource.Add(1, 10);
		mapSource.Add(2, 20);
		mapSource.Add(9, 90);
		mapSource.Remove(2);
		TMap<int, int> mapTarget;
		mapTarget.Add(4, 40);
		mapTarget.Add(5, 50);
		mapTarget = std::move(mapSource);
		Require(mapTarget.Num() == 2 && mapTarget[1] == 10 && mapTarget[9] == 90, "map move assignment must transfer key/value indices");
		Require(mapSource.Num() == 2 && mapSource[4] == 40 && mapSource[5] == 50, "map move source must preserve its replacement key/value indices");
		mapTarget.Add(6, 60);
		mapSource.Add(7, 70);
		Require(mapTarget[6] == 60 && mapTarget.GetValues().Num() == 3, "map move must preserve reusable value slots");
		Require(mapSource[7] == 70 && mapSource.GetKeys().Num() == 3, "both sides of map assignment must remain reusable");
		TMap<int, int> movedMap(std::move(mapTarget));
		Require(mapTarget.IsEmpty() && mapTarget.GetValues().IsEmpty(), "map move construction must empty keys and values together");
		mapTarget.Add(11, 110);
		Require(mapTarget.Num() == 1 && mapTarget[11] == 110, "moved-from map must accept new values");
		auto& sameMap = movedMap;
		movedMap = sameMap;
		Require(movedMap.Num() == 3 && movedMap[9] == 90, "map self-copy after move must preserve entries");
		TMap<int, int> copy(movedMap);
		copy.Remove(1);
		Require(movedMap.ContainsKey(1) && !copy.ContainsKey(1), "map copies must own independent buckets and values");
		TMap<int, int> zeroBuckets(0);
		zeroBuckets.Add(1, 10);
		zeroBuckets.Clear(0);
		zeroBuckets.Add(2, 20);
		Require(zeroBuckets.Num() == 1 && zeroBuckets[2] == 20, "map must remain insertable with a zero bucket request");
	}

	class OctreeDebugLines : public RHI::DebugContext
	{
	public:
		size_t NumBoxes() const { return m_lineVertices.Num() / 24; }

		size_t CountBox(const glm::ivec3& center, const glm::ivec3& extents) const
		{
			size_t count = 0;
			for (size_t i = 0; i < m_lineVertices.Num(); i += 24)
			{
				glm::vec3 minimum = m_lineVertices[i].m_position;
				glm::vec3 maximum = minimum;
				for (size_t j = 1; j < 24; ++j)
				{
					minimum = glm::min(minimum, m_lineVertices[i + j].m_position);
					maximum = glm::max(maximum, m_lineVertices[i + j].m_position);
				}
				count += minimum == glm::vec3(center - extents) && maximum == glm::vec3(center + extents);
			}
			return count;
		}
	};

	void TestSparseOctreeBoundsAndUpdates()
	{
		TOctree2<int> tree(glm::ivec3(0), 130, 4);
		Require(tree.Insert(glm::ivec3(0), glm::ivec3(10), 99), "spanning bounds must fit at the root");
		std::vector<glm::ivec3> positions;
		for (int i = 0; i < 16; ++i)
		{
			positions.emplace_back((i & 1) ? 20 + i : -20 - i, (i & 2) ? 24 : -24, (i & 4) ? 24 : -24);
			Require(tree.Insert(positions.back(), glm::ivec3(1), i), "sparse octree must retain every split element");
		}
		Require(tree.Insert(glm::ivec3(63, 20, 20), glm::ivec3(1), 100), "bounds outside a rounded child but inside its parent must remain in the parent");
		Require(tree.Num() == 18 && tree.NumNodes() > 1, "split must preserve element and node counts");
		OctreeDebugLines lines;
		tree.DrawOctree(lines);
		Require(lines.NumBoxes() == tree.Num() + tree.NumNodes(), "debug drawing must visit every node and element once");
		Require(lines.CountBox(glm::ivec3(0), glm::ivec3(10)) == 1, "splitting must not discard spanning bounds");
		Require(lines.CountBox(glm::ivec3(63, 20, 20), glm::ivec3(1)) == 1, "rounded child bounds must not lose parent elements");
		for (int i = 0; i < 16; ++i)
		{
			Require(tree.Contains(i) && lines.CountBox(positions[i], glm::ivec3(1)) == 1, "splitting must preserve each element's own bounds and reverse index");
		}
		Require(!tree.Insert(glm::ivec3(0), glm::ivec3(1), 0) && tree.Num() == 18, "duplicate insertion must not inflate counts or move an existing element");
		Require(tree.Update(glm::ivec3(45), glm::ivec3(2), 0) && tree.Num() == 18, "update across octants must not duplicate the element");
		OctreeDebugLines updated;
		tree.DrawOctree(updated);
		Require(updated.CountBox(positions[0], glm::ivec3(1)) == 0 && updated.CountBox(glm::ivec3(45), glm::ivec3(2)) == 1,
			"update must replace the old stored bounds");
		Require(!tree.Update(glm::ivec3(200), glm::ivec3(1), 0) && !tree.Contains(0) && tree.Num() == 17,
			"moving outside the root must remove the element and decrement its count");
		Require(tree.Update(glm::ivec3(-40), glm::ivec3(1), 0) && tree.Num() == 18, "update of a missing key must count its insertion");
		for (int i = 0; i < 16; ++i) Require(tree.Remove(i), "reverse index must remove every redistributed element");
		Require(tree.Remove(100) && !tree.Remove(100), "removal must be counted only once");
		tree.Resolve();
		Require(tree.Num() == 1 && tree.NumNodes() == 1 && tree.Contains(99), "empty children must collapse even when their parent retains spanning bounds");
		tree.Clear();
		Require(tree.Num() == 0 && tree.NumNodes() == 1 && !tree.Contains(99), "clear must retain an empty root with correct counts");
		Require(tree.Insert(glm::ivec3(0), glm::ivec3(1), 7), "a cleared sparse tree must remain insertable");

		TOctree2<int> spanning(glm::ivec3(0), 128, 4);
		for (int i = 0; i < 8; ++i) Require(spanning.Insert(glm::ivec3(0), glm::ivec3(10), i), "spanning fixture must insert");
		spanning.Resolve();
		Require(spanning.Num() == 8 && spanning.NumNodes() == 1, "resolving a spanning-only tree must retain all root elements");
		Require(spanning.Insert(glm::ivec3(24), glm::ivec3(1), 8) && spanning.Num() == 9 && spanning.NumNodes() > 1,
			"a collapsed parent above the split threshold must allocate occupied children again");
	}

	void TestSparseOctreeOwnership()
	{
		TOctree2<int> tree(glm::ivec3(0), 128, 4);
		for (int i = 0; i < 16; ++i) Require(tree.Insert(glm::ivec3(-24), glm::ivec3(1), i), "single-octant fixture must insert");
		const size_t nodes = tree.NumNodes();
		Require(nodes > 1 && nodes < 9, "sparse subdivision must allocate only occupied child octants");
		TOctree2<int> moved(std::move(tree));
		Require(moved.Num() == 16 && moved.NumNodes() == nodes && tree.Num() == 0 && tree.NumNodes() == 0,
			"move construction must transfer all sparse-tree counts");
		tree.Clear();
		tree.Resolve();
		OctreeDebugLines empty;
		tree.DrawOctree(empty);
		Require(empty.NumBoxes() == 0 && !tree.Insert(glm::ivec3(0), glm::ivec3(1), 42), "a moved-from rootless tree must remain safely clearable and queryable");
		TOctree2<int> singleNodeTree(glm::ivec3(100), 32, 4);
		Require(singleNodeTree.Insert(glm::ivec3(100), glm::ivec3(1), 99), "move-assignment destination fixture must insert");
		singleNodeTree = std::move(moved);
		Require(singleNodeTree.NumNodes() == nodes && singleNodeTree.Num() == 16 && singleNodeTree.Contains(15), "move assignment must transfer the reverse index and topology together");
		Require(moved.NumNodes() == 1 && moved.Num() == 1 && moved.Contains(99), "swap-style move assignment must leave a coherent source");
		TOctree2<int>::Swap(tree, singleNodeTree);
		Require(tree.Num() == 16 && tree.NumNodes() == nodes && singleNodeTree.NumNodes() == 0, "swap must transfer node counts to a rootless tree");
		for (int i = 0; i < 16; ++i) Require(tree.Remove(i), "moved sparse-tree nodes must retain their reverse index");
		tree.Resolve();
		Require(tree.Num() == 0 && tree.NumNodes() == 1, "all empty descendants must collapse after removal");
	}

	void TestOctreeCopyMoveAndReverseIndex()
	{
		TOctree<int> original(glm::ivec3(0), 128, 4);
		for (int i = 0; i < 16; ++i)
		{
			const glm::ivec3 position((i & 1) ? 24 : -24, (i & 2) ? 24 : -24, (i & 4) ? 24 : -24);
			Require(original.Insert(position, glm::ivec3(1), i), "octree fixture must insert every element");
		}
		Require(original.NumNodes() > 1, "octree fixture must exercise subdivided nodes");
		const size_t nodeCount = original.NumNodes();
		TOctree<int> copied(original);
		Require(copied.Num() == 16 && copied.NumNodes() == nodeCount, "octree copy must preserve counts and topology");
		for (int i = 0; i < 16; ++i) Require(copied.Contains(i), "octree copy must rebuild the reverse index");
		Require(copied.Update(glm::ivec3(40), glm::ivec3(1), 0) && copied.Num() == 16, "copy update must move, not duplicate, an existing element");
		Require(copied.Remove(1) && !copied.Contains(1) && copied.Num() == 15, "copy removal must use its own nodes");
		Require(original.Contains(1) && original.Num() == 16, "copy mutations must not change the source");
		auto& same = copied;
		copied = same;
		Require(copied.Num() == 15 && copied.Contains(0), "octree self-assignment must preserve its index");
		TOctree<int> assigned(glm::ivec3(100), 32, 4);
		assigned.Insert(glm::ivec3(100), glm::ivec3(1), 99);
		assigned = original;
		Require(!assigned.Contains(99) && assigned.Contains(15), "octree assignment must replace the old reverse index");
		TOctree<int> moved(std::move(assigned));
		Require(moved.NumNodes() == nodeCount && moved.Contains(15), "octree move must transfer node counts and reverse index");
		Require(assigned.Num() == 0 && assigned.NumNodes() == 0, "moved-from octree must not retain node counts");
		assigned = original;
		Require(assigned.NumNodes() == nodeCount && assigned.Remove(0), "copy assignment must work after move");
		TOctree<int> singleNodeTree;
		singleNodeTree.Insert(glm::ivec3(0), glm::ivec3(1), 99);
		TOctree<int>::Swap(singleNodeTree, moved);
		Require(singleNodeTree.NumNodes() == nodeCount && singleNodeTree.Contains(15), "swap must transfer the subdivided tree count");
		Require(moved.NumNodes() == 1 && moved.Contains(99), "swap must transfer the single-root tree count");
		singleNodeTree.Clear();
		Require(singleNodeTree.Num() == 0 && singleNodeTree.NumNodes() == 1 && !singleNodeTree.Contains(15), "clear after swap must leave one empty root");
	}
}

int main()
{
	try
	{
		TestVectorEraseRanges<int>();
		TestVectorEraseRanges<MoveOnly>();
		TestVectorEraseRanges<CopyOnly>();
		TestVectorConstructionAndAssignment();
		TestContainerAlignment();
		TestVectorInsertionAndRemoveFirst<int>();
		TestVectorInsertionAndRemoveFirst<CopyOnly>();
		TestListOwnershipAndSorting();
		TestIteratorInterfaces();
		TestMapExtractionAndConstIteration();
		TestMapValueEqualityAndSwap();
		TestPairConstruction();
		TestSetAndMapMoveContracts();
		TestOctreeCopyMoveAndReverseIndex();
		TestSparseOctreeBoundsAndUpdates();
		TestSparseOctreeOwnership();
		CheckLifetime(0);
		std::cout << "ContainerContractTests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ContainerContractTests failed: " << error.what() << '\n';
		return 1;
	}
}
