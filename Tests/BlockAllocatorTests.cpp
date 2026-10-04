#include "Memory/MemoryBlockAllocator.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace BlockAllocatorTests
{
	struct AllocatorStats
	{
		std::vector<size_t> m_capacities;
		size_t m_freedBlocks = 0;
	};

	class MisalignedAllocator
	{
	public:
		void* Allocate(size_t size)
		{
			m_stats->m_capacities.push_back(size);
			auto* base = static_cast<uint8_t*>(Sailor::Memory::MallocAllocator::allocate(size + 1, 64));
			return base + 1;
		}

		void Free(void* ptr, size_t)
		{
			++m_stats->m_freedBlocks;
			Sailor::Memory::MallocAllocator::free(static_cast<uint8_t*>(ptr) - 1);
		}

		AllocatorStats* m_stats = nullptr;
	};

	// Like a device-memory handle, this describes a range without allocating CPU bytes.
	struct LogicalPtr
	{
		size_t m_block = 0;
		size_t m_offset = 0;
		size_t m_size = 0;

		explicit operator bool() const { return m_block != 0; }
		LogicalPtr& operator=(std::nullptr_t)
		{
			*this = LogicalPtr{};
			return *this;
		}
	};

	class LogicalAllocator
	{
	public:
		LogicalPtr Allocate(size_t size)
		{
			m_stats->m_capacities.push_back(size);
			return { m_stats->m_capacities.size(), 0, size };
		}

		void Free(LogicalPtr, size_t)
		{
			++m_stats->m_freedBlocks;
		}

		AllocatorStats* m_stats = nullptr;
	};
}

namespace Sailor::Memory
{
	template<>
	inline BlockAllocatorTests::LogicalPtr GetPointer<BlockAllocatorTests::LogicalPtr>(
		const BlockAllocatorTests::LogicalPtr& block, size_t offset, size_t size)
	{
		return { block.m_block, offset, size };
	}

	template<>
	inline BlockAllocatorTests::LogicalPtr Shift<BlockAllocatorTests::LogicalPtr>(
		const BlockAllocatorTests::LogicalPtr& ptr, size_t offset)
	{
		return { ptr.m_block, ptr.m_offset + offset, ptr.m_size };
	}

	template<>
	inline bool Align<BlockAllocatorTests::LogicalPtr>(size_t size, size_t alignment,
		const BlockAllocatorTests::LogicalPtr& start, size_t capacity, uint32_t& alignmentOffset)
	{
		const size_t remainder = start.m_offset % alignment;
		const size_t padding = remainder ? alignment - remainder : 0;
		if (padding > capacity || size > capacity - padding)
		{
			return false;
		}
		alignmentOffset = static_cast<uint32_t>(padding);
		return true;
	}
}

namespace BlockAllocatorTests
{
	using Sailor::Memory::TBlockAllocator;

	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	void TestMisalignedBackingBlock()
	{
		AllocatorStats stats;
		{
			TBlockAllocator<MisalignedAllocator> allocator(64, 1, (std::numeric_limits<size_t>::max)());
			allocator.GetGlobalAllocator().m_stats = &stats;
			auto first = allocator.Allocate(64, 64);
			Require(stats.m_capacities.size() == 1 && stats.m_capacities.front() >= 127,
				"a new block must reserve room for payload and worst-case alignment padding");
			Require(first.m_alignmentOffset == 63 && reinterpret_cast<uintptr_t>(*first) % 64 == 0,
				"a misaligned backing address must still produce an aligned allocation");
			Require(first.m_offset + first.m_alignmentOffset + first.m_size <= stats.m_capacities.front(),
				"the aligned payload must fit within the backing allocation");
			std::memset(*first, 0x5a, first.m_size);
			auto* address = *first;
			allocator.Free(first);
			Require(!first, "Free must clear the allocation handle");
			auto reused = allocator.Allocate(64, 64);
			Require(*reused == address && stats.m_capacities.size() == 1,
				"new and reused blocks must use the same alignment calculation");
			allocator.Free(reused);
		}
		Require(stats.m_freedBlocks == 1, "the retained backing block must be freed once at destruction");
	}

	void TestReuseAndCoalescing()
	{
		AllocatorStats stats;
		{
			TBlockAllocator<MisalignedAllocator> allocator(256, 1, (std::numeric_limits<size_t>::max)());
			allocator.GetGlobalAllocator().m_stats = &stats;
			auto first = allocator.Allocate(17, 16);
			auto second = allocator.Allocate(23, 32);
			auto third = allocator.Allocate(40, 16);
			Require(first.m_blockIndex == second.m_blockIndex && second.m_blockIndex == third.m_blockIndex,
				"small aligned allocations must share the same backing block");
			auto* firstAddress = *first;
			allocator.Free(first);
			auto replacement = allocator.Allocate(17, 16);
			Require(*replacement == firstAddress, "a freed hole must be reusable before allocating another block");
			allocator.Free(replacement);
			allocator.Free(third);
			allocator.Free(second);

			auto wholeBlock = allocator.Allocate(256, 1);
			Require(wholeBlock.m_offset == 0 && wholeBlock.m_alignmentOffset == 0 && stats.m_capacities.size() == 1,
				"freeing both neighbors must coalesce the full range, including alignment padding");
			allocator.Free(wholeBlock);
			auto reused = allocator.Allocate(256, 1);
			Require(reused.m_blockIndex == 0 && stats.m_capacities.size() == 1,
				"freeing a full block must return it to the allocation search");
			allocator.Free(reused);
		}
		Require(stats.m_freedBlocks == 1, "coalescing must not allocate or release extra backing blocks");
	}

	void TestReleasedBlockSlotReuse()
	{
		AllocatorStats stats;
		{
			TBlockAllocator<MisalignedAllocator> allocator(128, 16, 0);
			allocator.GetGlobalAllocator().m_stats = &stats;
			auto first = allocator.Allocate(128, 1);
			auto second = allocator.Allocate(128, 1);
			const uint32_t releasedIndex = second.m_blockIndex;
			allocator.Free(first);
			allocator.Free(second);
			Require(stats.m_freedBlocks == 1 && allocator.GetOccupiedSpace() == 128,
				"an empty excess block must release its backing storage");
			auto larger = allocator.Allocate(256, 1);
			Require(larger.m_blockIndex == releasedIndex && stats.m_capacities.back() == 256,
				"a released block slot must accept a new backing allocation and layout");
			allocator.Free(larger);
		}
		Require(stats.m_capacities.size() == 3 && stats.m_freedBlocks == 3,
			"block moves, slot reuse and destruction must free each backing allocation once");
	}

	void TestLargeLogicalAllocations()
	{
		static_assert(sizeof(size_t) >= 8);
		constexpr size_t fourGiB = size_t{ 1 } << 32;
		constexpr size_t request = fourGiB + 123;
		AllocatorStats stats;
		{
			// Zero average size keeps partially used blocks searchable for this range test.
			TBlockAllocator<LogicalAllocator, LogicalPtr> allocator(1024, 0, (std::numeric_limits<size_t>::max)());
			allocator.GetGlobalAllocator().m_stats = &stats;
			auto large = allocator.Allocate(request, 256);
			const size_t capacity = stats.m_capacities.front();
			Require(capacity >= request + 255 && large.m_size == request && allocator.GetOccupiedSpace() == capacity,
				"requests and backing capacities above four GiB must not narrow to uint32");
			Require((*large).m_size == request && (*large).m_offset == 0,
				"the returned typed pointer must preserve its full logical size");

			auto tail = allocator.Allocate(64, 192);
			Require(stats.m_capacities.size() == 1 && tail.m_offset > fourGiB && (*tail).m_offset % 192 == 0,
				"existing layout offsets must stay wide and honor device-style alignment");
			Require(tail.m_offset + tail.m_alignmentOffset + tail.m_size <= capacity,
				"the tail allocation must remain inside the large logical block");
			allocator.Free(large);
			allocator.Free(tail);
			auto wholeBlock = allocator.Allocate(capacity, 1);
			Require(wholeBlock.m_size == capacity && wholeBlock.m_offset == 0 && stats.m_capacities.size() == 1,
				"large freed ranges must coalesce without narrowing offsets or sizes");
			allocator.Free(wholeBlock);
		}
		{
			constexpr size_t configuredCapacity = fourGiB + 4096;
			TBlockAllocator<LogicalAllocator, LogicalPtr> allocator(configuredCapacity, 64, 0);
			allocator.GetGlobalAllocator().m_stats = &stats;
			auto small = allocator.Allocate(16, 64);
			Require(stats.m_capacities.back() == configuredCapacity && allocator.GetOccupiedSpace() == configuredCapacity,
				"a configured block size above four GiB must not narrow for a small request");
			allocator.Free(small);
		}
		Require(stats.m_capacities.size() == 2 && stats.m_freedBlocks == 2,
			"logical backing allocations must be released normally without reserving CPU gigabytes");
	}
}

int main()
{
	try
	{
		BlockAllocatorTests::TestMisalignedBackingBlock();
		BlockAllocatorTests::TestReuseAndCoalescing();
		BlockAllocatorTests::TestReleasedBlockSlotReuse();
		BlockAllocatorTests::TestLargeLogicalAllocations();
		std::cout << "BlockAllocatorTests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "BlockAllocatorTests failed: " << error.what() << '\n';
		return 1;
	}
}
