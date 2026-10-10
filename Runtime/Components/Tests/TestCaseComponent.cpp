#include "Components/Tests/TestCaseComponent.h"
#include "Platform/Time.h"
#include "AssetRegistry/AssetRegistry.h"
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
	if (!m_bBeginPlayCalled)
	{
		Component::EndPlay();
		return;
	}

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

bool TestCaseComponent::CaptureScreenshot(const ReadbackFrame& frame, std::string_view outputFilename, std::string& outError)
{
	TVector<glm::u8vec4> pixels;
	if (!frame.CopySrgbPixels(pixels))
	{
		outError = "Capture pixel format or row layout is invalid.";
		return false;
	}
	return SaveImageToPng(pixels, glm::uvec2(frame.m_extent), outputFilename, outError);
}

bool TestCaseComponent::SaveImageToPng(const TVector<glm::u8vec4>& data, glm::uvec2 extent, std::string_view outputFilename, std::string& outError)
{
	if (extent.x == 0 || extent.y == 0)
	{
		outError = "Image extent is invalid.";
		return false;
	}

	if (data.Num() < (size_t)extent.x * (size_t)extent.y)
	{
		outError = "Image data is incomplete.";
		return false;
	}

	std::error_code ec;
	const std::filesystem::path outputPath = Workspace::PathFromUtf8(GetTestsCacheFolder()) / Workspace::PathFromUtf8(EnsurePngExtension(std::string(outputFilename)));
	std::filesystem::create_directories(outputPath.parent_path(), ec);

	constexpr uint32_t Channels = 4;
	if (!stbi_write_png(Workspace::PathToUtf8(outputPath).c_str(), (int)extent.x, (int)extent.y, Channels, data.GetData(), (int)extent.x * (int)Channels))
	{
		outError = "Cannot write image to " + Workspace::PathToUtf8(outputPath);
		return false;
	}

	outError.clear();
	return true;
}

void TestCaseComponent::AddJournalEvent(const char* eventName, const std::string& message, int64_t durationMs) const
{
	AppendJournalEvent(eventName, message, durationMs);
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
