#include "Math/Math.h"

#include <cstdlib>

using namespace Sailor;

// Reversed Z matrix
//https://nlguillemot.wordpress.com/2016/12/07/reversed-z-in-opengl/
glm::mat4 Math::PerspectiveInfiniteRH(float fovRadians, float aspectWbyH, float zNear)
{
	float f = 1.0f / tan(fovRadians / 2.0f);
	return glm::mat4(
		f / aspectWbyH, 0.0f, 0.0f, 0.0f,
		0.0f, f, 0.0f, 0.0f,
		0.0f, 0.0f, 0.0f, -1.0f,
		0.0f, 0.0f, zNear, 0.0f);
}

// Reversed Z matrix
glm::mat4 Math::PerspectiveRH(float fovRadians, float aspectWbyH, float zNear, float zFar)
{
	return glm::perspectiveRH(fovRadians, aspectWbyH, zFar, zNear);
}

glm::vec4 Utils::LinearToSRGB(const glm::u8vec4& linearRGB)
{
	return vec4(LinearToSRGB(vec4(linearRGB)));
}

glm::vec4 Utils::SRGBToLinear(const glm::u8vec4& srgbIn)
{
	return vec4(SRGBToLinear(vec4(srgbIn)));
}

glm::vec4 Utils::LinearToSRGB(const glm::vec4& linearRGB)
{
	return vec4(LinearToSRGB(glm::vec3(linearRGB)), linearRGB.a);
}

glm::vec4 Utils::SRGBToLinear(const glm::vec4& srgbIn)
{
	return vec4(SRGBToLinear(glm::vec3(srgbIn)), srgbIn.a);
}

glm::vec3 Utils::LinearToSRGB(const glm::vec3& linearRGB)
{
	auto cutoff = glm::lessThan(linearRGB, glm::vec3(0.0031308f));
	glm::vec3 higher = glm::vec3(1.055f) * glm::pow(linearRGB, glm::vec3(1.f / 2.4f)) - glm::vec3(0.055f);
	glm::vec3 lower = linearRGB * glm::vec3(12.92f);

	return glm::mix(higher, lower, cutoff);
}

glm::u8vec4 Utils::LinearToSRGB8(const glm::vec4& linearRGBA)
{
	const glm::u8vec3 rgb(glm::clamp(LinearToSRGB(glm::vec3(linearRGBA)) * 255.0f, 0.0f, 255.0f));
	const auto alpha = static_cast<uint8_t>(glm::round(glm::clamp(linearRGBA.a, 0.0f, 1.0f) * 255.0f));
	return glm::u8vec4(rgb, alpha);
}

glm::vec3 Utils::SRGBToLinear(const glm::vec3& srgbIn)
{
	glm::vec3 bLess = glm::step(glm::vec3(0.04045f), srgbIn);
	return glm::mix(srgbIn / glm::vec3(12.92f), glm::pow((srgbIn + glm::vec3(0.055f)) / glm::vec3(1.055f), glm::vec3(2.4f)), bLess);
}

DWORD Utils::GetRandomColorHex()
{
	const uint8_t r = (uint8_t)(rand() % 255);
	const uint8_t g = (uint8_t)(rand() % 255);
	const uint8_t b = (uint8_t)(rand() % 255);
	return (DWORD)(r | (g << 8) | (b << 16));
}

int32_t Utils::CalculateJulianDayNumber(int32_t year, int32_t month, int32_t day)
{
	// Formula coming from Wikipedia.
	int32_t a = (month - 14) / 12;
	int32_t jdn = (1461 * (year + 4800 + a)) / 4 +
		(367 * (month - 2 - 12 * a)) / 12 -
		(3 * ((year + 4900 + a) / 100)) / 4 +
		day - 32075;

	return jdn;
}

double Utils::CalculateJulianDate(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute, int32_t second)
{
	int32_t jdn = CalculateJulianDayNumber(year, month, day);

	double jd = jdn + ((hour - 12.0) / 24.0) + (minute / 1440.0) + (second / 86400.0);
	return jd;
}

double Utils::CalculateJulianCenturyDate(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute, int32_t second)
{
	double jd = CalculateJulianDate(year, month, day, hour, minute, second);
	return (jd - s_j2000) / 36525.0;
}

glm::vec3 Utils::ConvertToEuclidean(float rightAscension, float declination, float radialDistance)
{
	const float cosd = cosf(declination);
	glm::vec3 out{};

	out.x = radialDistance * sinf(rightAscension) * cosd;
	out.y = radialDistance * cosf(rightAscension) * cosd;
	out.z = radialDistance * sinf(declination);

	return out;
}
