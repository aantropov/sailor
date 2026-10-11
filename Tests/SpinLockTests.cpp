#include "Core/SpinLock.h"

#include <atomic>
#include <barrier>
#include <iostream>
#include <thread>

int main()
{
	Sailor::SpinLock lock;
	if (!lock.TryLock())
	{
		std::cerr << "TryLock failed on an unlocked lock\n";
		return 1;
	}
	if (lock.TryLock())
	{
		std::cerr << "TryLock acquired an already held lock\n";
		return 1;
	}
	lock.Unlock();

	constexpr uint32_t rounds = 10000;
	std::barrier start(3);
	std::barrier attempted(3);
	std::barrier released(3);
	std::atomic<uint32_t> owners{ 0 };
	bool singleOwner = true;

	auto compete = [&]()
	{
		for (uint32_t round = 0; round < rounds; ++round)
		{
			start.arrive_and_wait();
			const bool acquired = lock.TryLock();
			if (acquired)
			{
				owners.fetch_add(1);
			}
			// The owner holds the lock until the other attempt has returned.
			attempted.arrive_and_wait();
			if (acquired)
			{
				lock.Unlock();
			}
			released.arrive_and_wait();
		}
	};

	std::thread first(compete);
	std::thread second(compete);
	for (uint32_t round = 0; round < rounds; ++round)
	{
		start.arrive_and_wait();
		attempted.arrive_and_wait();
		singleOwner &= owners.load() == 1;
		released.arrive_and_wait();
		owners.store(0);
	}
	first.join();
	second.join();
	if (!singleOwner)
	{
		std::cerr << "Concurrent attempts did not select one owner\n";
		return 1;
	}
	std::cout << "SpinLock contracts passed\n";
	return 0;
}
