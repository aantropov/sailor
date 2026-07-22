#include "Math/Bounds.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

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

	bool IsNear(const glm::vec3& lhs, const glm::vec3& rhs, float epsilon = 0.0001f)
	{
		return std::abs(lhs.x - rhs.x) <= epsilon &&
			std::abs(lhs.y - rhs.y) <= epsilon &&
			std::abs(lhs.z - rhs.z) <= epsilon;
	}

	void TestValidityRejectsSentinelAndInvertedBounds()
	{
		const Math::AABB defaultBounds;
		Require(!defaultBounds.IsValid(), "default sentinel bounds must be invalid");

		Math::AABB invertedBounds;
		invertedBounds.m_min = glm::vec3(-1.0f, 2.0f, -1.0f);
		invertedBounds.m_max = glm::vec3(1.0f, 1.0f, 1.0f);
		Require(!invertedBounds.IsValid(), "bounds with min greater than max on any axis must be invalid");

		Math::AABB pointBounds;
		pointBounds.m_min = glm::vec3(3.0f, -2.0f, 5.0f);
		pointBounds.m_max = pointBounds.m_min;
		Require(pointBounds.IsValid(), "finite zero-volume bounds must remain valid");
	}

	void TestValidityRejectsNonFiniteBounds()
	{
		Math::AABB bounds(glm::vec3(0.0f), glm::vec3(1.0f));
		Require(bounds.IsValid(), "finite ordered bounds must be valid");

		bounds.m_min.x = std::numeric_limits<float>::quiet_NaN();
		Require(!bounds.IsValid(), "bounds containing NaN must be invalid");

		bounds = Math::AABB(glm::vec3(0.0f), glm::vec3(1.0f));
		bounds.m_max.z = std::numeric_limits<float>::infinity();
		Require(!bounds.IsValid(), "bounds containing infinity must be invalid");

		bounds.m_max.z = -std::numeric_limits<float>::infinity();
		Require(!bounds.IsValid(), "bounds containing negative infinity must be invalid");
	}

	void TestTransformPreservesAllNegativeBounds()
	{
		Math::AABB bounds(glm::vec3(1.0f), glm::vec3(1.0f));
		const glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(-10.0f, -20.0f, -30.0f));

		bounds.Apply(transform);

		Require(bounds.IsValid(), "transformed all-negative bounds must remain valid");
		Require(IsNear(bounds.m_min, glm::vec3(-10.0f, -20.0f, -30.0f)),
			"transformed all-negative minimum must include every corner");
		Require(IsNear(bounds.m_max, glm::vec3(-8.0f, -18.0f, -28.0f)),
			"transformed all-negative maximum must include every corner");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "ValidityRejectsSentinelAndInvertedBounds", TestValidityRejectsSentinelAndInvertedBounds },
		{ "ValidityRejectsNonFiniteBounds", TestValidityRejectsNonFiniteBounds },
		{ "TransformPreservesAllNegativeBounds", TestTransformPreservesAllNegativeBounds },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.first << ": " << error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
