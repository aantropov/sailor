#pragma once

#include "Core/Defines.h"
#include "Transform.h"

#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/common.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace glm;

namespace Sailor::Math
{
	template<glm::length_t L, typename T, qualifier Q>
	__forceinline bool AllFinite(const vec<L, T, Q>& value)
	{
		for (glm::length_t i = 0; i < L; i++)
		{
			if (!std::isfinite(static_cast<double>(value[i])))
			{
				return false;
			}
		}

		return true;
	}

	template<glm::length_t C, glm::length_t R, typename T, qualifier Q>
	__forceinline bool AllFinite(const mat<C, R, T, Q>& value)
	{
		for (glm::length_t column = 0; column < C; ++column)
		{
			if (!AllFinite(value[column]))
			{
				return false;
			}
		}

		return true;
	}

	template<typename T, qualifier Q>
	__forceinline bool AllFinite(const qua<T, Q>& value)
	{
		return AllFinite(vec<4, T, Q>(value.x, value.y, value.z, value.w));
	}

	template<glm::length_t C, glm::length_t R, typename T, qualifier Q>
	__forceinline bool AreExactlyEqual(
		const mat<C, R, T, Q>& lhs,
		const mat<C, R, T, Q>& rhs)
	{
		for (glm::length_t column = 0; column < C; ++column)
		{
			for (glm::length_t row = 0; row < R; ++row)
			{
				if (lhs[column][row] != rhs[column][row])
				{
					return false;
				}
			}
		}

		return true;
	}

	template<glm::length_t C, glm::length_t R, typename T, qualifier Q>
	__forceinline bool AreNearlyEqual(
		const mat<C, R, T, Q>& lhs,
		const mat<C, R, T, Q>& rhs,
		T tolerance = static_cast<T>(0.0001))
	{
		for (glm::length_t column = 0; column < C; ++column)
		{
			for (glm::length_t row = 0; row < R; ++row)
			{
				const T scale = (std::max)({
					static_cast<T>(1),
					std::abs(lhs[column][row]),
					std::abs(rhs[column][row]) });
				if (std::abs(lhs[column][row] - rhs[column][row]) >
					tolerance * scale)
				{
					return false;
				}
			}
		}

		return true;
	}

	const float Pi = 3.1415926f;

	const glm::vec3 vec3_Zero = glm::vec3(0, 0, 0);
	const glm::vec3 vec3_One = glm::vec3(1, 1, 1);

	const glm::vec3 vec3_Up = glm::vec3(0, 1, 0);
	const glm::vec3 vec3_Forward = glm::vec3(0, 0, -1);
	const glm::vec3 vec3_Right = glm::vec3(1, 0, 0);

	const glm::vec3 vec3_Backward = -vec3_Forward;
	const glm::vec3 vec3_Down = -vec3_Up;
	const glm::vec3 vec3_Left = -vec3_Right;

	const glm::vec4 vec4_Zero = glm::vec4(0, 0, 0, 0);
	const glm::vec4 vec4_One = glm::vec4(1, 1, 1, 1);

	const glm::vec4 vec4_Up = glm::vec4(0, 1, 0, 0);
	const glm::vec4 vec4_Forward = glm::vec4(0, 0, -1, 0);
	const glm::vec4 vec4_Right = glm::vec4(1, 0, 0, 0);

	const glm::vec4 vec4_Back = -vec4_Forward;
	const glm::vec4 vec4_Down = -vec4_Up;
	const glm::vec4 vec4_Left = -vec4_Right;

	template<typename T, qualifier Q>
	__forceinline vec<3, T, Q> SafeNormalize(const vec<3, T, Q>& value, const vec<3, T, Q>& fallback = vec<3, T, Q>(0))
	{
		const T len2 = glm::dot(value, value);
		if (len2 <= std::numeric_limits<T>::epsilon() || !AllFinite(value))
		{
			return fallback;
		}

		return value * glm::inversesqrt(len2);
	}

	template<typename T, qualifier Q>
	__forceinline vec<4, T, Q> SafeNormalize(const vec<4, T, Q>& value, const vec<4, T, Q>& fallback = vec<4, T, Q>(0))
	{
		const T len2 = glm::dot(value, value);
		if (len2 <= std::numeric_limits<T>::epsilon() || !AllFinite(value))
		{
			return fallback;
		}

		return value * glm::inversesqrt(len2);
	}

	const quat quat_Identity = quat(1.0, 0.0, 0.0, 0.0);
	const mat4 mat4_Identity = mat4(1);

	template<typename T>
	SAILOR_API __forceinline T UpperPowOf2(T v)
	{
		v--;
		v |= v >> 1;
		v |= v >> 2;
		v |= v >> 4;
		v |= v >> 8;
		v |= v >> 16;
		v++;
		return v;
	}

	template<typename T>
	T Lerp(const T& a, const T& b, float t) { return a + (b - a) * t; }

	template<>
	SAILOR_API Transform Lerp<Transform>(const Transform& a, const Transform& b, float t);

	// Maps between ranges without clamping; minValue and maxValue must differ.
	inline float Remap(float value, float minValue, float maxValue, float newMinValue, float newMaxValue)
	{
		return newMinValue + (value - minValue) / (maxValue - minValue) * (newMaxValue - newMinValue);
	}

	SAILOR_API __forceinline glm::mat4 PerspectiveInfiniteRH(float fovRadians, float aspectWbyH, float zNear);
	SAILOR_API __forceinline glm::mat4 PerspectiveRH(float fovRadians, float aspectWbyH, float zNear, float zFar);
}

#if defined(min)
#undef min
#define min(a,b) (a < b ? a : b)
#endif

#if defined(max)
#undef max
#define max(a,b) (a < b ? b : a)
#endif

namespace Sailor::Utils
{
	SAILOR_API DWORD GetRandomColorHex();

	SAILOR_API __forceinline glm::vec4 LinearToSRGB(const glm::u8vec4& linearRGB);
	SAILOR_API __forceinline glm::vec4 SRGBToLinear(const glm::u8vec4& srgbIn);

	SAILOR_API __forceinline glm::vec4 LinearToSRGB(const glm::vec4& linearRGB);
	SAILOR_API __forceinline glm::vec4 SRGBToLinear(const glm::vec4& srgbIn);

	SAILOR_API __forceinline glm::vec3 LinearToSRGB(const glm::vec3& linearRGB);
	SAILOR_API glm::u8vec4 LinearToSRGB8(const glm::vec4& linearRGBA);
	SAILOR_API __forceinline glm::vec3 SRGBToLinear(const glm::vec3& srgbIn);

	static constexpr int32_t s_j2000 = 2451545;

	SAILOR_API int32_t CalculateJulianDayNumber(int32_t year, int32_t month, int32_t day);
	SAILOR_API double CalculateJulianDate(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute, int32_t second);

	// Julian centuries since January 1, 2000
	SAILOR_API double CalculateJulianCenturyDate(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute, int32_t second);
	SAILOR_API glm::vec3 ConvertToEuclidean(float rightAscension, float declination, float radialDistance);
}
