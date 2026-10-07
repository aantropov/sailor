#include "Memory/HeapAllocator.h"
#include "Memory/LockFreeHeapAllocator.h"
#include "Memory/MallocAllocator.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <numeric>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace
{
	using Clock = std::chrono::steady_clock;

	struct Settings
	{
		size_t m_count = 4096;
		size_t m_minSize = 1;
		size_t m_maxSize = 4096;
		size_t m_iterations = 5;
	};

	std::optional<size_t> ProcessPrivateBytes()
	{
#if defined(_WIN32)
		PROCESS_MEMORY_COUNTERS_EX counters{};
		counters.cb = sizeof(counters);
		if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), counters.cb))
			return static_cast<size_t>(counters.PrivateUsage);
#endif
		return {};
	}

	void PrintBytes(std::optional<size_t> bytes)
	{
		if (bytes) std::cout << *bytes;
		else std::cout << "unavailable";
	}

	template<typename Allocator>
	bool Measure(std::string_view allocatorName, std::string_view scenario, size_t iteration,
		const std::vector<size_t>& sizes, const std::vector<size_t>& order)
	{
		Allocator allocator;
		std::vector<uint8_t*> blocks(sizes.size(), nullptr);
		const auto before = ProcessPrivateBytes();
		std::optional<size_t> live;
		Clock::duration allocateTime{}, freeTime{};
		uint64_t checksum = 0;
		bool bSucceeded = true;
		auto allocate = [&](size_t begin, size_t end)
		{
			const auto start = Clock::now();
			for (size_t i = begin; i < end; ++i)
			{
				blocks[i] = static_cast<uint8_t*>(allocator.Allocate(sizes[i], 16));
				if (!blocks[i]) { bSucceeded = false; break; }
				blocks[i][0] = static_cast<uint8_t>(i);
				blocks[i][sizes[i] - 1] = static_cast<uint8_t>(i);
			}
			allocateTime += Clock::now() - start;
		};
		auto release = [&](size_t begin, size_t end, size_t step)
		{
			// Consume touched bytes outside the timed free phase. This is a workload
			// checksum, not a replacement for allocator correctness tests.
			for (size_t index = begin; index < end; index += step)
			{
				const size_t i = order[index];
				if (!blocks[i]) continue;
				bSucceeded &= blocks[i][0] == static_cast<uint8_t>(i) && blocks[i][sizes[i] - 1] == static_cast<uint8_t>(i);
				checksum += blocks[i][0] + blocks[i][sizes[i] - 1];
			}
			const auto start = Clock::now();
			for (size_t index = begin; index < end; index += step)
			{
				const size_t i = order[index];
				if (!blocks[i]) continue;
				allocator.Free(blocks[i]);
				blocks[i] = nullptr;
			}
			freeTime += Clock::now() - start;
		};
		if (scenario == "interleaved")
		{
			const size_t half = sizes.size() / 2;
			allocate(0, half);
			release(0, half, 2);
			allocate(half, sizes.size());
		}
		else allocate(0, sizes.size());
		live = ProcessPrivateBytes();
		release(0, sizes.size(), 1);
		const uint64_t expectedChecksum = std::accumulate(order.begin(), order.end(), uint64_t(0),
			[](uint64_t sum, size_t i) { return sum + 2u * static_cast<uint8_t>(i); });
		bSucceeded &= checksum == expectedChecksum;

		std::cout << allocatorName << ',' << scenario << ',' << iteration << ',' << sizes.size() << ','
			<< std::accumulate(sizes.begin(), sizes.end(), size_t(0)) << ','
			<< std::chrono::duration<double, std::micro>(allocateTime).count() << ','
			<< std::chrono::duration<double, std::micro>(freeTime).count() << ',';
		PrintBytes(before);
		std::cout << ',';
		PrintBytes(live);
		std::cout << ',' << checksum << ',' << (bSucceeded ? "measured" : "failed") << '\n';
		return bSucceeded;
	}

	bool ReadSettings(int argc, char** argv, Settings& settings)
	{
		for (int i = 1; i < argc; i += 2)
		{
			if (i + 1 == argc) return false;
			const std::string_view option(argv[i]);
			const std::string_view value(argv[i + 1]);
			size_t number = 0;
			const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
			if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number == 0) return false;
			if (option == "--count") settings.m_count = number;
			else if (option == "--min-size") settings.m_minSize = number;
			else if (option == "--max-size") settings.m_maxSize = number;
			else if (option == "--iterations") settings.m_iterations = number;
			else return false;
		}
		return settings.m_minSize <= settings.m_maxSize;
	}
}

int main(int argc, char** argv)
{
	constexpr std::string_view Usage = "SailorMemoryBenchmark [--count N] [--min-size bytes] [--max-size bytes] [--iterations N]\n";
	if (argc == 2 && std::string_view(argv[1]) == "--help") { std::cout << Usage; return 0; }
	Settings settings;
	if (!ReadSettings(argc, argv, settings)) { std::cerr << Usage; return 2; }
	std::mt19937 random(0);
	std::uniform_int_distribution<size_t> size(settings.m_minSize, settings.m_maxSize);
	std::vector<size_t> sizes(settings.m_count), order(settings.m_count), shuffled;
	for (auto& bytes : sizes) bytes = size(random);
	std::iota(order.begin(), order.end(), size_t(0));
	shuffled = order;
	std::shuffle(shuffled.begin(), shuffled.end(), random);
	std::cout << "allocator,scenario,iteration,allocations,payload_bytes,allocate_touch_us,free_us,process_private_before_bytes,process_private_live_bytes,checksum,result\n";
	for (std::string_view scenario : { "simple", "shuffle", "interleaved" })
	{
		const auto& freeOrder = scenario == "shuffle" ? shuffled : order;
		for (size_t iteration = 0; iteration < settings.m_iterations; ++iteration)
		{
			if (!Measure<Sailor::Memory::HeapAllocator>("HeapAllocator", scenario, iteration, sizes, freeOrder) ||
				!Measure<Sailor::Memory::LockFreeHeapAllocator>("LockFreeHeapAllocator", scenario, iteration, sizes, freeOrder) ||
				!Measure<Sailor::Memory::MallocAllocator>("MallocAllocator", scenario, iteration, sizes, freeOrder)) return 1;
		}
	}
	return 0;
}
