#include <array>
#include <atomic>
#include <barrier>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "Memory/SharedPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "Memory/MemoryPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "Engine/Object.h"
#include "Containers/Vector.h"

using namespace Sailor;

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	struct LifetimeProbe
	{
		LifetimeProbe()
		{
			++s_liveCount;
		}

		explicit LifetimeProbe(int value) : m_value(value)
		{
			++s_liveCount;
		}

		~LifetimeProbe()
		{
			--s_liveCount;
			++s_destroyedCount;
			s_destroyedOn = std::this_thread::get_id();
		}

		static void Reset()
		{
			s_liveCount = 0;
			s_destroyedCount = 0;
			s_destroyedOn = {};
		}

		static inline int s_liveCount = 0;
		static inline int s_destroyedCount = 0;
		static inline std::thread::id s_destroyedOn;
		int m_value = 0;
	};

	struct VectorLifetimeProbe
	{
		VectorLifetimeProbe()
		{
			++s_liveCount;
		}

		VectorLifetimeProbe(const VectorLifetimeProbe&) = delete;
		VectorLifetimeProbe& operator=(const VectorLifetimeProbe&) = delete;

		VectorLifetimeProbe(VectorLifetimeProbe&&) noexcept
		{
			++s_liveCount;
			++s_moveCount;
		}

		~VectorLifetimeProbe()
		{
			--s_liveCount;
			++s_destroyedCount;
		}

		static void Reset()
		{
			s_liveCount = 0;
			s_moveCount = 0;
			s_destroyedCount = 0;
		}

		static inline int s_liveCount = 0;
		static inline int s_moveCount = 0;
		static inline int s_destroyedCount = 0;
	};

	struct CountingAllocator : Memory::MallocAllocator
	{
		static void* allocate(size_t size, size_t alignment = 8)
		{
			++s_allocations;
			return Memory::MallocAllocator::allocate(size, alignment);
		}

		static void free(void* ptr, size_t size = 0)
		{
			if (ptr)
			{
				++s_frees;
			}
			Memory::MallocAllocator::free(ptr, size);
		}

		void* Allocate(size_t size, size_t alignment = 8) { return allocate(size, alignment); }
		void Free(void* ptr, size_t size = 0) { free(ptr, size); }

		static inline int s_allocations = 0;
		static inline int s_frees = 0;
	};

	struct PointerBase
	{
		virtual ~PointerBase() = default;
		int m_value = 29;
	};

	struct OtherPointerBase
	{
		virtual ~OtherPointerBase() = default;
		int m_padding = 0;
	};

	struct PointerDerived : OtherPointerBase, PointerBase
	{
		~PointerDerived() override { ++s_destroyed; }
		static inline int s_destroyed = 0;
	};

	void TestWeakPointerConversionsAndAllocator()
	{
		using DerivedPtr = TSharedPtr<PointerDerived, CountingAllocator>;
		using BasePtr = TSharedPtr<PointerBase, CountingAllocator>;
		using DerivedWeak = TWeakPtr<PointerDerived, CountingAllocator>;
		using BaseWeak = TWeakPtr<PointerBase, CountingAllocator>;
		static_assert(std::is_same_v<decltype(DerivedPtr::Make()), DerivedPtr>);
		static_assert(std::is_same_v<decltype(BaseWeak{}.TryLock()), BasePtr>);
		static_assert(!std::is_constructible_v<TWeakPtr<PointerBase>, DerivedWeak>);

		CountingAllocator::s_allocations = CountingAllocator::s_frees = 0;
		PointerDerived::s_destroyed = 0;
		BaseWeak observer;
		{
			auto derived = DerivedPtr::Make();
			BasePtr base = derived;
			DerivedWeak weakDerived = derived;
			BaseWeak copied = weakDerived;
			observer = std::move(weakDerived);
			Require(!weakDerived.IsValid(), "derived weak move should empty the source");
			Require(observer == copied && observer.Lock()->m_value == 29,
				"derived weak conversion should preserve the adjusted base pointer and owner");
			Require(std::hash<BaseWeak>{}(observer) == std::hash<BaseWeak>{}(copied),
				"equal custom-allocator weak pointers should have equal hashes");

			BaseWeak direct = derived;
			BasePtr moved = std::move(derived);
			Require(!derived && direct.Lock() == moved, "shared derived move should preserve the allocator and ownership");
			observer = std::move(copied);
			Require(!copied.IsValid(), "moving another weak pointer to the same owner should empty the source");
			auto& same = observer;
			observer = std::move(same);
			Require(observer.Lock() == base, "weak self-move should retain its owner");
		}
		Require(PointerDerived::s_destroyed == 1 && !observer.TryLock(),
			"the last shared owner should destroy the derived object without promoting an expired weak pointer");
		Require(CountingAllocator::s_allocations == 1 && CountingAllocator::s_frees == 0,
			"a weak pointer should retain the original allocator's control block");
		observer.Clear();
		Require(CountingAllocator::s_frees == 1, "the last weak pointer should free the control block with its allocator");

		auto value = TSharedPtr<int, CountingAllocator>::Make(7);
		TSharedPtr<const int, CountingAllocator> constValue = value;
		TWeakPtr<const int, CountingAllocator> weakConst = value;
		Require(*constValue == 7 && *weakConst.Lock() == 7, "const conversions should also support scalar pointees");
	}

	alignas(64) std::array<std::byte, 64> g_reusedStorage;

	struct ReusedStorageProbe
	{
		explicit ReusedStorageProbe(int value) : m_value(value) {}
		static void* operator new(size_t) { return g_reusedStorage.data(); }
		static void operator delete(void*) {}
		int m_value;
	};

	void TestWeakPointerReusedAddress()
	{
		auto original = TSharedPtr<ReusedStorageProbe>::Make(1);
		TWeakPtr<ReusedStorageProbe> oldOwner = original;
		TWeakPtr<ReusedStorageProbe> assigned = original;
		original.Clear();
		Require(!oldOwner.TryLock(), "the original generation should expire before storage reuse");

		auto replacement = TSharedPtr<ReusedStorageProbe>::Make(2);
		TWeakPtr<ReusedStorageProbe> newOwner = replacement;
		Require(oldOwner != newOwner, "weak equality should distinguish owners at a reused address");
		assigned = replacement;
		Require(assigned == newOwner && assigned.Lock()->m_value == 2,
			"weak assignment should replace an expired owner even at the same address");
		std::unordered_set<TWeakPtr<ReusedStorageProbe>> owners{ oldOwner, newOwner, assigned };
		Require(owners.size() == 2, "weak key equality and hash should preserve distinct object generations");
	}

	struct ConcurrentProbe
	{
		ConcurrentProbe(std::atomic<uint32_t>& destroyed, uint32_t value) : m_destroyed(destroyed), m_value(value) {}
		~ConcurrentProbe() { ++m_destroyed; }
		std::atomic<uint32_t>& m_destroyed;
		uint32_t m_value;
	};

	void TestWeakPromotionAgainstLastOwner()
	{
		constexpr uint32_t iterations = 4000;
		std::atomic<uint32_t> destroyed = 0;
		std::atomic<bool> invalidLifetime = false;
		std::barrier phase(2);
		TWeakPtr<ConcurrentProbe> observer;
		std::jthread consumer([&]()
			{
				for (uint32_t i = 0; i < iterations; ++i)
				{
					phase.arrive_and_wait();
					if (auto retained = observer.TryLock())
					{
						if (retained->m_value != i || destroyed.load() != i)
						{
							invalidLifetime = true;
						}
					}
					phase.arrive_and_wait();
				}
			});
		for (uint32_t i = 0; i < iterations; ++i)
		{
			auto owner = TSharedPtr<ConcurrentProbe>::Make(destroyed, i);
			observer = owner;
			phase.arrive_and_wait();
			owner.Clear();
			phase.arrive_and_wait();
			if (destroyed.load() != i + 1 || observer.TryLock())
			{
				invalidLifetime = true;
			}
		}
		consumer.join();
		Require(!invalidLifetime, "weak promotion must retain a live owner or fail, without reviving a destroyed object");
		Require(destroyed == iterations, "each concurrent object should be destroyed exactly once");
		TWeakPtr<ConcurrentProbe> empty;
		Require(!empty.TryLock(), "an empty weak pointer should not promote");

		auto owner = TSharedPtr<ConcurrentProbe>::Make(destroyed, iterations);
		observer = owner;
		auto retained = observer.TryLock();
		owner.Clear();
		Require(retained && destroyed == iterations, "successful promotion should retain the object after its original owner exits");
		retained.Clear();
		Require(destroyed == iterations + 1 && !observer.TryLock(), "a retained object should expire after its last promoted owner exits");
	}

	struct alignas(128) AlignedValue
	{
		explicit AlignedValue(int value = 0) : m_value(value) {}
		int m_value;
	};

	template<typename TAllocator>
	void CheckAllocatorAlignment(TAllocator& allocator)
	{
		constexpr std::array<size_t, 8> alignments{ 1, 2, 4, 8, 16, 32, 64, 128 };
		constexpr std::array<size_t, 9> sizes{ 1, 7, 15, 63, 127, 250, 257, 511, 2053 };
		std::array<void*, alignments.size() * sizes.size()> allocations{};
		size_t index = 0;
		for (size_t alignment : alignments)
		{
			for (size_t size : sizes)
			{
				void* ptr = allocator.Allocate(size, alignment);
				Require(ptr && reinterpret_cast<uintptr_t>(ptr) % alignment == 0,
					"allocator should honor the requested payload alignment");
				std::memset(ptr, static_cast<int>(index), size);
				allocations[index++] = ptr;
			}
		}
		for (size_t i = allocations.size(); i > 0; --i)
		{
			const auto* data = static_cast<const uint8_t*>(allocations[i - 1]);
			for (size_t j = 0; j < sizes[(i - 1) % sizes.size()]; ++j)
			{
				Require(data[j] == static_cast<uint8_t>(i - 1), "allocations with different alignment should not overlap");
			}
			allocator.Free(allocations[i - 1]);
		}
		auto* value = Memory::New<AlignedValue>(allocator, 43);
		Require(reinterpret_cast<uintptr_t>(value) % alignof(AlignedValue) == 0 && value->m_value == 43,
			"typed allocation should use alignof(T) and forward constructor arguments");
		Memory::Delete(allocator, value);
		value = Memory::New<AlignedValue>(allocator);
		Require(reinterpret_cast<uintptr_t>(value) % alignof(AlignedValue) == 0 && value->m_value == 0,
			"default typed allocation should also use alignof(T)");
		Memory::Delete(allocator, value);
	}

	void TestAllocatorAlignmentAndGrowth()
	{
		Memory::MallocAllocator malloc;
		Memory::HeapAllocator heap;
		Memory::LockFreeHeapAllocator shared;
		CheckAllocatorAlignment(malloc);
		CheckAllocatorAlignment(heap);
		CheckAllocatorAlignment(shared);

		Memory::HeapAllocator growHeap;
		void* growing = growHeap.Allocate(257, 64);
		std::memset(growing, 37, 257);
		Require(growHeap.Reallocate(growing, 513, 64), "a pool block should grow into its free successor");
		void* following = growHeap.Allocate(517, 32);
		Require(reinterpret_cast<uintptr_t>(following) % 32 == 0, "growth should keep the next header and payload aligned");
		for (size_t i = 0; i < 257; ++i)
		{
			Require(static_cast<uint8_t*>(growing)[i] == 37, "in-place growth should preserve existing bytes");
		}
		growHeap.Free(following);
		growHeap.Free(growing);

		void* crossThread = shared.Allocate(257, 128);
		std::memset(crossThread, 61, 257);
		bool preserved = true;
		std::jthread releaser([crossThread, &preserved]()
			{
				Memory::LockFreeHeapAllocator::reallocate(crossThread, 513, 128);
				for (size_t i = 0; i < 257; ++i)
				{
					preserved = preserved && static_cast<uint8_t*>(crossThread)[i] == 61;
				}
				Memory::LockFreeHeapAllocator::free(crossThread);
			});
		releaser.join();
		Require(preserved, "cross-thread reallocation/free should retain the original allocator and payload");
	}

	void TestInlineAllocatorReuse()
	{
		CountingAllocator::s_allocations = CountingAllocator::s_frees = 0;
		Memory::TInlineAllocator<512, CountingAllocator> allocator;
		void* expected = nullptr;
		for (int i = 0; i < 100; ++i)
		{
			void* first = allocator.Allocate(17, 64);
			Require(reinterpret_cast<uintptr_t>(first) % 64 == 0, "inline payload should be aligned after its header");
			Require(!expected || first == expected, "freeing the last inline block should restore its padding too");
			expected = first;
			void* second = allocator.Allocate(7, 32);
			Require(!allocator.Reallocate(first, 40, 64), "only the last inline block can grow in place");
			Require(allocator.Reallocate(second, 63, 32), "the last inline block should grow in remaining stack space");
			allocator.Free(second);
			Require(allocator.Reallocate(first, 80, 64), "freeing the top block should expose the previous block for growth");
			allocator.Free(first);
		}
		Require(CountingAllocator::s_allocations == 0, "repeated inline grow/free should not leak stack space into fallback allocations");
		void* fallback = allocator.Allocate(4096, 128);
		Require(reinterpret_cast<uintptr_t>(fallback) % 128 == 0, "inline fallback should preserve alignment");
		allocator.Free(fallback);
		Require(CountingAllocator::s_allocations == 1 && CountingAllocator::s_frees == 1, "fallback memory should be released by its allocator");
	}

	struct alignas(128) ObjectProbe : Object
	{
		ObjectProbe(int& destroyed, int value) : m_destroyed(destroyed), m_value(value) {}
		~ObjectProbe() override { ++m_destroyed; }
		int& m_destroyed;
		int m_value;
	};

	struct UnreadyObjectProbe : ObjectProbe
	{
		using ObjectProbe::ObjectProbe;
		bool IsValid() const override { return false; }
	};

	void TestDestroyUnreadyObject()
	{
		using namespace Sailor::Memory;
		int destroyed = 0;
		auto allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::LocalMemory_SingleThread);
		auto object = TObjectPtr<UnreadyObjectProbe>::Make(allocator, destroyed, 42);
		auto retained = object;
		Require(object && !object.IsValid(), "a live object may not be ready for gameplay");
		object.DestroyObject(allocator);
		Require(destroyed == 1 && !object && !retained, "explicit destruction must invalidate every handle regardless of gameplay readiness");
		retained.DestroyObject(allocator);
		object.Clear();
		retained.Clear();
		Require(destroyed == 1, "clearing destroyed handles must not destroy the object twice");
	}

	void TestObjectPointerAllocatorAssignment()
	{
		using namespace Memory;
		auto firstAllocator = ObjectAllocatorPtr::Make(EAllocationPolicy::LocalMemory_SingleThread);
		auto secondAllocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
		int destroyed = 0;
		auto first = TObjectPtr<ObjectProbe>::Make(firstAllocator, destroyed, 1);
		auto second = TObjectPtr<ObjectProbe>::Make(secondAllocator, destroyed, 2);
		Require(reinterpret_cast<uintptr_t>(first.GetRawPtr()) % alignof(ObjectProbe) == 0,
			"object allocation should preserve its concrete alignment");
		first = second;
		Require(destroyed == 1 && first->m_value == 2, "cross-allocator assignment should destroy the old object with its original owner");
		second.Clear();
		auto& same = first;
		first = same;
		first = std::move(same);
		Require(destroyed == 1 && first->m_value == 2, "object self-assignment should preserve ownership");
		first.DestroyObject(secondAllocator);
		Require(destroyed == 2 && !first, "the assigned object should still have its allocator for explicit destruction");
		auto copiedExpired = first;
		Require(copiedExpired.IsInited() && !copiedExpired, "copying a destroyed object should retain its expired control block");
		auto expired = std::move(first);
		Require(!first.IsInited() && expired.IsInited(), "moving an explicitly destroyed object should transfer its retained control block");
		expired.Clear();
		copiedExpired.Clear();

		auto old = TObjectPtr<ObjectProbe>::Make(firstAllocator, destroyed, 3);
		auto retained = old;
		auto replacement = TObjectPtr<ObjectProbe>::Make(firstAllocator, destroyed, 4);
		old = replacement;
		Require(destroyed == 2 && retained->m_value == 3 && old->m_value == 4,
			"same-allocator assignment should not destroy an object held by another handle");
		retained.Clear();
		Require(destroyed == 3, "the last old handle should destroy its own object");
		TWeakPtr<ObjectAllocator> allocatorLifetime = firstAllocator;
		firstAllocator.Clear();
		replacement.Clear();
		Require(allocatorLifetime.TryLock().IsValid(), "a surviving object handle should retain its allocator");
		old.Clear();
		Require(destroyed == 4 && !allocatorLifetime.TryLock(), "the final handle should release both its object and allocator");
	}

	struct ManagedAllocator
	{
		explicit ManagedAllocator(int& freed) : m_freed(freed) {}
		~ManagedAllocator()
		{
			for (void* ptr : m_allocations)
			{
				if (ptr)
				{
					Memory::MallocAllocator::free(ptr);
					++m_freed;
				}
			}
		}
		Memory::TMemoryPtr<void*> Allocate(uint32_t index)
		{
			m_allocations[index] = Memory::MallocAllocator::allocate(32);
			return { 0, 0, 32, m_allocations[index], index };
		}
		void Free(const Memory::TMemoryPtr<void*>& ptr)
		{
			Memory::MallocAllocator::free(m_allocations[ptr.m_blockIndex]);
			m_allocations[ptr.m_blockIndex] = nullptr;
			++m_freed;
		}
		int& m_freed;
		std::array<void*, 2> m_allocations{};
	};

	void TestManagedMemoryMoveOnly()
	{
		using Managed = Memory::TManagedMemory<void*, ManagedAllocator>;
		static_assert(!std::is_copy_constructible_v<Managed> && !std::is_copy_assignable_v<Managed>);
		static_assert(std::is_nothrow_move_constructible_v<Managed> && std::is_nothrow_move_assignable_v<Managed>);
		int freed = 0;
		auto allocator = TSharedPtr<ManagedAllocator>::Make(freed);
		{
			Managed first(allocator->Allocate(0), allocator);
			Managed moved(std::move(first));
			Managed replacement(allocator->Allocate(1), allocator);
			replacement = std::move(moved);
			Require(freed == 1 && replacement.Get().m_blockIndex == 0, "move assignment should free only the replaced allocation");
			auto& same = replacement;
			replacement = std::move(same);
			Require(freed == 1, "managed self-move should preserve its allocation");
		}
		Require(freed == 2, "moved managed payloads should free every allocation exactly once");
		{
			Managed expired(allocator->Allocate(0), allocator);
			allocator.Clear();
			Require(freed == 3, "the allocator should release outstanding storage at shutdown");
		}
		Require(freed == 3, "a payload must not call an expired allocator");
	}

	void TestSharedPtrConstObserversAndComparison()
	{
		LifetimeProbe::Reset();
		{
			auto shared = TSharedPtr<LifetimeProbe>::Make(3);
			const TSharedPtr<LifetimeProbe> constShared = shared;
			constShared->m_value = 7;
			(*constShared).m_value = 11;

			Require(shared->m_value == 11, "const shared pointer should preserve mutable pointee access");
			Require((shared <=> constShared) == std::strong_ordering::equal, "shared copies should compare equal");

			auto other = TSharedPtr<LifetimeProbe>::Make(5);
			Require((shared <=> other) != std::strong_ordering::equal, "different shared objects should not compare equal");
			Require(LifetimeProbe::s_liveCount == 2, "shared pointers should retain both live objects");
		}

		Require(LifetimeProbe::s_liveCount == 0, "shared pointers should destroy their pointees exactly once");
		Require(LifetimeProbe::s_destroyedCount == 2, "shared pointer destruction count should match allocations");
	}

	void TestSharedPtrRetainedAcrossThreads()
	{
		LifetimeProbe::Reset();
		auto writable = TSharedPtr<LifetimeProbe>::Make(19);
		TSharedPtr<const LifetimeProbe> frame = std::move(writable);
		Require(!writable && !frame.IsShared(), "publishing a const frame should transfer ownership");

		bool bConsumerRetained = false;
		std::jthread consumer([frame, &bConsumerRetained]()
			{
				auto copy = frame;
				bConsumerRetained = copy.IsShared() && copy->m_value == 19;
			});
		consumer.join();

		Require(bConsumerRetained, "consumer should retain the immutable frame");
		Require(!frame.IsShared(), "owner should be the last reference after the consumer completes");
		Require(LifetimeProbe::s_liveCount == 1 && LifetimeProbe::s_destroyedCount == 0,
			"consumer completion must not destroy the owner's retained frame");
		frame.Clear();
		Require(LifetimeProbe::s_liveCount == 0 && LifetimeProbe::s_destroyedCount == 1,
			"the last owner should destroy the frame exactly once");
		Require(LifetimeProbe::s_destroyedOn == std::this_thread::get_id(),
			"retained frame should be destroyed on the owner thread");
	}

	void TestUniquePtrScalarMoveAndClear()
	{
		LifetimeProbe::Reset();
		{
			auto pointer = TUniquePtr<LifetimeProbe>::Make(17);
			auto moved = std::move(pointer);
			Require(!pointer && moved->m_value == 17, "scalar move construction should transfer ownership");

			auto& samePointer = moved;
			moved = std::move(samePointer);
			Require(moved && moved->m_value == 17, "scalar self-move should preserve ownership");
			Require(LifetimeProbe::s_liveCount == 1 && LifetimeProbe::s_destroyedCount == 0,
				"scalar self-move must neither delete nor lose the object");

			auto replacement = TUniquePtr<LifetimeProbe>::Make(23);
			replacement = std::move(moved);
			Require(!moved && replacement->m_value == 17, "scalar move assignment should transfer ownership");
			Require(LifetimeProbe::s_liveCount == 1 && LifetimeProbe::s_destroyedCount == 1,
				"scalar move assignment should destroy the replaced object");
			replacement.Clear();
			replacement.Clear();
		}
		Require(LifetimeProbe::s_liveCount == 0 && LifetimeProbe::s_destroyedCount == 2,
			"cleared scalar owners should destroy each object exactly once");
	}

	void TestUniquePtrScalarRelease()
	{
		LifetimeProbe::Reset();
		LifetimeProbe* released = nullptr;
		{
			auto pointer = TUniquePtr<LifetimeProbe>::Make(17);
			released = pointer.Release();
			Require(!pointer, "released scalar unique pointer should become empty");
			Require(released != nullptr && released->m_value == 17, "scalar release should return the owned pointee");
		}

		Require(LifetimeProbe::s_liveCount == 1, "released scalar pointee should outlive its former owner");
		delete released;
		Require(LifetimeProbe::s_liveCount == 0, "released scalar pointee should remain manually deletable");
		Require(LifetimeProbe::s_destroyedCount == 1, "scalar release should not double-delete the pointee");
	}

	void TestUniquePtrArrayLifetimeMoveAndRelease()
	{
		LifetimeProbe::Reset();
		LifetimeProbe* released = nullptr;
		{
			auto array = TUniquePtr<LifetimeProbe[]>::Make(3);
			Require(LifetimeProbe::s_liveCount == 3, "array unique pointer should construct every element");
			array[1].m_value = 23;

			auto moved = std::move(array);
			Require(!array && moved[1].m_value == 23, "array move construction should transfer ownership");

			moved = std::move(moved);
			Require(moved && moved[1].m_value == 23, "array self-move assignment should preserve ownership");

			auto replacement = TUniquePtr<LifetimeProbe[]>::Make(1);
			replacement = std::move(moved);
			Require(!moved && replacement[1].m_value == 23, "array move assignment should replace and transfer ownership");
			Require(LifetimeProbe::s_liveCount == 3, "array move assignment should destroy the replaced allocation");

			released = replacement.Release();
			Require(!replacement && released[1].m_value == 23, "array release should return the allocation and clear ownership");
		}

		Require(LifetimeProbe::s_liveCount == 3, "released array should outlive its former owner");
		delete[] released;
		Require(LifetimeProbe::s_liveCount == 0, "released array should remain manually deletable");
		Require(LifetimeProbe::s_destroyedCount == 4, "array move and release should destroy each element exactly once");
	}

	void TestUniquePtrArrayValueInitialization()
	{
		auto values = TUniquePtr<uint32_t[]>::Make(4);
		for (size_t i = 0; i < 4; ++i)
		{
			Require(values[i] == 0, "array Make should value-initialize fundamental elements");
		}
	}

	void TestVectorReserveMovesOnlyLiveElements()
	{
		VectorLifetimeProbe::Reset();
		{
			// Force relocation; the heap allocator may grow the block in place.
			TVector<VectorLifetimeProbe, Memory::MallocAllocator> values;
			values.Reserve(8);
			values.Emplace();
			values.Reserve(16);

			Require(VectorLifetimeProbe::s_liveCount == 1,
				"vector reserve should preserve exactly one live element");
			Require(VectorLifetimeProbe::s_moveCount == 1,
				"vector reserve should move Num elements rather than Capacity elements");
			Require(VectorLifetimeProbe::s_destroyedCount == 1,
				"vector reserve should destroy only the initialized source elements");
		}

		Require(VectorLifetimeProbe::s_liveCount == 0,
			"vector destruction should release the remaining live element");
		Require(VectorLifetimeProbe::s_destroyedCount == 2,
			"vector growth and destruction should destroy each constructed element once");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "WeakPointerConversionsAndAllocator", TestWeakPointerConversionsAndAllocator },
		{ "WeakPointerReusedAddress", TestWeakPointerReusedAddress },
		{ "WeakPromotionAgainstLastOwner", TestWeakPromotionAgainstLastOwner },
		{ "AllocatorAlignmentAndGrowth", TestAllocatorAlignmentAndGrowth },
		{ "InlineAllocatorReuse", TestInlineAllocatorReuse },
		{ "ObjectPointerAllocatorAssignment", TestObjectPointerAllocatorAssignment },
		{ "DestroyUnreadyObject", TestDestroyUnreadyObject },
		{ "ManagedMemoryMoveOnly", TestManagedMemoryMoveOnly },
		{ "SharedPtrConstObserversAndComparison", TestSharedPtrConstObserversAndComparison },
		{ "SharedPtrRetainedAcrossThreads", TestSharedPtrRetainedAcrossThreads },
		{ "UniquePtrScalarMoveAndClear", TestUniquePtrScalarMoveAndClear },
		{ "UniquePtrScalarRelease", TestUniquePtrScalarRelease },
		{ "UniquePtrArrayLifetimeMoveAndRelease", TestUniquePtrArrayLifetimeMoveAndRelease },
		{ "UniquePtrArrayValueInitialization", TestUniquePtrArrayValueInitialization },
		{ "VectorReserveMovesOnlyLiveElements", TestVectorReserveMovesOnlyLiveElements },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& e)
		{
			std::cerr << "[FAIL] " << test.first << ": " << e.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
