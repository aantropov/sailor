#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "Core/Utils.h"

using Sailor::Utils::Timer;
using namespace std::chrono_literals;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	void TestEmptyAndClear()
	{
		Timer timer;
		Require(timer.ResultMs() == 0 && timer.ResultAccumulatedMs() == 0,
			"an unused timer should report zero duration");
		timer.Stop();
		Require(timer.ResultMs() == 0 && timer.ResultAccumulatedMs() == 0,
			"stopping an unused timer should not create an interval");

		timer.Start();
		std::this_thread::sleep_for(10ms);
		timer.Clear();
		Require(!timer.m_bIsStarted && timer.ResultMs() == 0 && timer.ResultAccumulatedMs() == 0,
			"clearing a running timer should discard the active interval and stop it");
		timer.Stop();
		Require(timer.ResultAccumulatedMs() == 0, "stop after clear should not accumulate time since the clock origin");

		timer.Start();
		std::this_thread::sleep_for(10ms);
		timer.Stop();
		Require(timer.ResultMs() >= 5, "a cleared timer should be reusable");
		timer.Clear();
		Require(timer.ResultMs() == 0 && timer.ResultAccumulatedMs() == 0,
			"clear should discard completed intervals too");
	}

	void TestElapsedAndAccumulation()
	{
		Timer timer;
		timer.Start();
		std::this_thread::sleep_for(10ms);
		const int64_t running = timer.ResultMs();
		Require(running >= 5 && timer.ResultAccumulatedMs() >= running,
			"running results should include the elapsed active interval");
		timer.Stop();
		const int64_t first = timer.ResultMs();
		Require(first >= running && timer.ResultAccumulatedMs() == first,
			"the first completed interval should equal the accumulated result");

		std::this_thread::sleep_for(10ms);
		timer.Stop();
		Require(timer.ResultMs() == first && timer.ResultAccumulatedMs() == first,
			"stopped results should stay constant and repeated stop must not count the interval twice");

		timer.Start();
		std::this_thread::sleep_for(10ms);
		Require(timer.ResultAccumulatedMs() >= first + 5,
			"accumulation should include completed intervals while a new one is running");
		timer.Stop();
		const int64_t second = timer.ResultMs();
		const int64_t accumulated = timer.ResultAccumulatedMs();
		Require(second >= 5 && accumulated >= first + second && accumulated <= first + second + 1,
			"accumulation should add intervals with at most one millisecond of rounding difference");
	}

	void TestEpochTimeHelpers()
	{
		const auto before = std::chrono::system_clock::now().time_since_epoch();
		const int64_t milliseconds = Sailor::Utils::GetCurrentTimeMs();
		const int64_t microseconds = Sailor::Utils::GetCurrentTimeMicro();
		const int64_t nanoseconds = Sailor::Utils::GetCurrentTimeNano();
		const auto after = std::chrono::system_clock::now().time_since_epoch();
		Require(milliseconds >= std::chrono::duration_cast<std::chrono::milliseconds>(before).count() &&
			milliseconds <= std::chrono::duration_cast<std::chrono::milliseconds>(after).count(),
			"GetCurrentTimeMs should remain an epoch timestamp");
		Require(microseconds >= std::chrono::duration_cast<std::chrono::microseconds>(before).count() &&
			microseconds <= std::chrono::duration_cast<std::chrono::microseconds>(after).count(),
			"GetCurrentTimeMicro should remain an epoch timestamp");
		Require(nanoseconds >= std::chrono::duration_cast<std::chrono::nanoseconds>(before).count() &&
			nanoseconds <= std::chrono::duration_cast<std::chrono::nanoseconds>(after).count(),
			"GetCurrentTimeNano should remain an epoch timestamp");
	}
}

int main()
{
	try
	{
		TestEmptyAndClear();
		TestElapsedAndAccumulation();
		TestEpochTimeHelpers();
		std::cout << "[PASS] TimerContractTests\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "[FAIL] TimerContractTests: " << error.what() << '\n';
		return 1;
	}
	return 0;
}
