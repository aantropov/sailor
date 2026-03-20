#include "Components/Tests/TestCaseComponent.h"
#include "AssetRegistry/AssetRegistry.h"
#include "FrameGraph/CopyTextureToRamNode.h"
#include "RHI/Renderer.h"
#include "RHI/Buffer.h"
#include "RHI/Texture.h"
#include "Engine/InstanceId.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Engine/EngineLoop.h"
#include <yaml-cpp/yaml.h>
#include <stb_image_write.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cstring>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	std::string EnsurePngExtension(std::string filename)
	{
		if (!filename.ends_with(".png"))
		{
			filename += ".png";
		}

		return filename;
	}

	static float HalfToFloat(uint16_t h)
	{
		const uint32_t sign = (uint32_t)(h & 0x8000u) << 16u;
		uint32_t exp = (h >> 10u) & 0x1Fu;
		uint32_t mant = h & 0x03FFu;

		uint32_t out = 0;
		if (exp == 0)
		{
			if (mant == 0)
			{
				out = sign;
			}
			else
			{
				exp = 1;
				while ((mant & 0x0400u) == 0)
				{
					mant <<= 1u;
					--exp;
				}
				mant &= 0x03FFu;
				out = sign | ((exp + 112u) << 23u) | (mant << 13u);
			}
		}
		else if (exp == 31)
		{
			out = sign | 0x7F800000u | (mant << 13u);
		}
		else
		{
			out = sign | ((exp + 112u) << 23u) | (mant << 13u);
		}

		float result = 0.0f;
		std::memcpy(&result, &out, sizeof(float));
		return result;
	}

	static glm::vec4 DecodeR16G16B16A16_SFLOAT(const uint8_t* src)
	{
		const uint16_t* halfs = reinterpret_cast<const uint16_t*>(src);
		return glm::vec4(HalfToFloat(halfs[0]), HalfToFloat(halfs[1]), HalfToFloat(halfs[2]), HalfToFloat(halfs[3]));
	}
}

void TestCaseComponent::BeginPlay()
{
	Component::BeginPlay();

	m_testRunId = GenerateTestRunId();
	m_startTimeMs = Utils::GetCurrentTimeMs();
	m_startedAtUtc = GetCurrentTimeIso8601Utc();

	if (m_testName.empty())
	{
		m_testName = GetOwner().IsValid() ? GetOwner()->GetName() : GetTestType();
	}

	AppendJournalEvent("TestStart", "", 0);
}

void TestCaseComponent::EndPlay()
{
	if (!m_bFinished)
	{
		Finish(false, "Test ended before completion.");
	}

	Component::EndPlay();
}

void TestCaseComponent::Finish(bool bPassed, const std::string& message)
{
	if (m_bFinished)
	{
		return;
	}

	m_bFinished = true;

	const int64_t durationMs = (std::max)(int64_t(0), Utils::GetCurrentTimeMs() - m_startTimeMs);
	AppendJournalEvent(bPassed ? "TestPassed" : "TestFailed", message, durationMs);

	if (!bPassed)
	{
		App::SetExitCode(2);
	}

	if (m_bQuitAfterFinish)
	{
		if (auto engineLoop = App::GetSubmodule<EngineLoop>(); engineLoop && GetWorld())
		{
			engineLoop->ExitWorld(GetWorld());
		}
		else
		{
			App::Stop();
		}
	}
}

void TestCaseComponent::MarkPassed()
{
	Finish(true);
}

void TestCaseComponent::MarkFailed(const std::string& message)
{
	Finish(false, message);
}

std::string TestCaseComponent::GetTestsCacheFolder()
{
	return AssetRegistry::GetCacheFolder() + "Tests/";
}

bool TestCaseComponent::CaptureScreenshot(const std::string& outputFilename, std::string& outError)
{
	auto renderer = App::GetSubmodule<Renderer>();
	if (!renderer || !renderer->GetFrameGraph())
	{
		outError = "Renderer frame graph is not available.";
		return false;
	}

	auto snapshotNode = renderer->GetFrameGraph()->GetRHI()->GetGraphNode("CopyTextureToRam");
	auto snapshot = snapshotNode.DynamicCast<Framegraph::CopyTextureToRamNode>();
	if (!snapshot)
	{
		outError = "CopyTextureToRam node is not available.";
		return false;
	}

	auto cpuRam = snapshot->GetBuffer();
	auto texture = snapshot->GetTexture();
	if (!cpuRam || !texture)
	{
		outError = "Screenshot readback resources are not ready yet.";
		return false;
	}

	const glm::ivec2 extent = texture->GetExtent();
	if (extent.x <= 0 || extent.y <= 0)
	{
		outError = "Screenshot texture extent is invalid.";
		return false;
	}

	const auto* ptr = reinterpret_cast<const uint8_t*>(cpuRam->GetPointer());
	if (!ptr)
	{
		outError = "Screenshot readback buffer is not mapped.";
		return false;
	}

	constexpr size_t pixelStride = sizeof(uint16_t) * 4u;
	TVector<glm::u8vec3> outSrgb((size_t)extent.x * (size_t)extent.y);
	for (int y = 0; y < extent.y; y++)
	{
		for (int x = 0; x < extent.x; x++)
		{
			const uint32_t index = x + y * extent.x;
			const glm::vec4 rgba = DecodeR16G16B16A16_SFLOAT(ptr + (size_t)index * pixelStride);
			const glm::vec3 value = glm::clamp(Utils::LinearToSRGB(glm::vec3(rgba)), glm::vec3(0.0f), glm::vec3(1.0f));
			outSrgb[index] = glm::u8vec3(value * 255.0f);
		}
	}

	std::error_code ec;
	const std::filesystem::path outputPath = std::filesystem::path(GetTestsCacheFolder()) / EnsurePngExtension(outputFilename);
	std::filesystem::create_directories(outputPath.parent_path(), ec);

	constexpr uint32_t Channels = 3;
	if (!stbi_write_png(outputPath.string().c_str(), extent.x, extent.y, Channels, outSrgb.GetData(), extent.x * Channels))
	{
		outError = "Cannot write screenshot to " + outputPath.string();
		return false;
	}

	outError.clear();
	return true;
}

void TestCaseComponent::AppendJournalEvent(const char* eventName, const std::string& message, int64_t durationMs) const
{
	std::error_code ec;
	const std::filesystem::path testsPath(GetTestsCacheFolder());
	std::filesystem::create_directories(testsPath, ec);

	YAML::Emitter out;
	out << YAML::BeginMap;
	out << YAML::Key << "event" << YAML::Value << eventName;
	out << YAML::Key << "testRunId" << YAML::Value << m_testRunId;
	out << YAML::Key << "testName" << YAML::Value << m_testName;
	out << YAML::Key << "testType" << YAML::Value << GetTestType();
	out << YAML::Key << "worldName" << YAML::Value << (GetWorld() ? GetWorld()->GetName() : "unknown");
	out << YAML::Key << "worldPath" << YAML::Value << (App::GetLoadedWorldPath().empty() ? "unknown" : App::GetLoadedWorldPath());
	out << YAML::Key << "buildConfig" << YAML::Value << App::GetBuildConfig();
	out << YAML::Key << "engineVersion" << YAML::Value << App::GetEngineVersion();
	out << YAML::Key << "startedAtUtc" << YAML::Value << m_startedAtUtc;
	out << YAML::Key << "recordedAtUtc" << YAML::Value << GetCurrentTimeIso8601Utc();
	out << YAML::Key << "durationMs" << YAML::Value << durationMs;
	if (!message.empty())
	{
		out << YAML::Key << "message" << YAML::Value << message;
	}
	out << YAML::EndMap;

	std::ofstream journal(testsPath / "testJournal.yaml", std::ios::out | std::ios::app);
	journal << "---\n" << out.c_str() << "\n";
}

std::string TestCaseComponent::GenerateTestRunId()
{
	return InstanceId::GenerateNewComponentId(InstanceId::Invalid).ToString();
}

std::string TestCaseComponent::GetCurrentTimeIso8601Utc()
{
	const auto now = std::chrono::system_clock::now();
	const auto tt = std::chrono::system_clock::to_time_t(now);

	std::tm utc{};
#if defined(_WIN32)
	gmtime_s(&utc, &tt);
#else
	gmtime_r(&tt, &utc);
#endif

	std::ostringstream stream;
	stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
	return stream.str();
}
