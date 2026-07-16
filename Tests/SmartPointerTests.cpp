#include <compare>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "Memory/SharedPtr.hpp"
#include "Memory/UniquePtr.hpp"

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
		}

		static void Reset()
		{
			s_liveCount = 0;
			s_destroyedCount = 0;
		}

		static inline int s_liveCount = 0;
		static inline int s_destroyedCount = 0;
		int m_value = 0;
	};

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
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "SharedPtrConstObserversAndComparison", TestSharedPtrConstObserversAndComparison },
		{ "UniquePtrScalarRelease", TestUniquePtrScalarRelease },
		{ "UniquePtrArrayLifetimeMoveAndRelease", TestUniquePtrArrayLifetimeMoveAndRelease },
		{ "UniquePtrArrayValueInitialization", TestUniquePtrArrayValueInitialization },
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
