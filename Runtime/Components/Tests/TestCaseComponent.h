#pragma once
#include "Sailor.h"
#include "Components/Component.h"

namespace Sailor
{
	class TestCaseComponent : public Component
	{
		SAILOR_REFLECTABLE(TestCaseComponent)

	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void EndPlay() override;


	public:

		SAILOR_API const std::string& GetTestName() const { return m_testName; }
		SAILOR_API void SetTestName(const std::string& value) { m_testName = value; }

		SAILOR_API bool GetQuitAfterFinish() const { return m_bQuitAfterFinish; }
		SAILOR_API void SetQuitAfterFinish(bool bValue) { m_bQuitAfterFinish = bValue; }

	protected:

		std::string m_testName;
		bool m_bQuitAfterFinish = true;

		SAILOR_API virtual const char* GetTestType() const { return "TestCase"; }

		SAILOR_API void Finish(bool bPassed, const std::string& message = "");
		SAILOR_API void MarkPassed();
		SAILOR_API void MarkFailed(const std::string& message);

	public:
		SAILOR_API void AddJournalEvent(const char* eventName, const std::string& message, int64_t durationMs = 0) const;

		SAILOR_API bool IsFinished() const { return m_bFinished; }
		SAILOR_API const std::string& GetTestRunId() const { return m_testRunId; }
		SAILOR_API int64_t GetStartTimeMs() const { return m_startTimeMs; }

		SAILOR_API static std::string GetTestsCacheFolder();
		SAILOR_API static bool CaptureScreenshot(const std::string& outputFilename, std::string& outError);
		SAILOR_API static bool SaveImageToPng(const TVector<glm::u8vec4>& data, glm::uvec2 extent, const std::string& outputFilename, std::string& outError);

	private:

		void AppendJournalEvent(const char* eventName, const std::string& message, int64_t durationMs) const;
		static std::string GenerateTestRunId();
		static std::string GetCurrentTimeIso8601Utc();

		std::string m_testRunId;
		std::string m_startedAtUtc;
		int64_t m_startTimeMs = 0;
		bool m_bFinished = false;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::TestCaseComponent, bases<Sailor::Component>),

	func(GetTestName, property("testName"), SkipCDO()),
	func(SetTestName, property("testName"), SkipCDO()),

	func(GetQuitAfterFinish, property("quitAfterFinish"), SkipCDO()),
	func(SetQuitAfterFinish, property("quitAfterFinish"), SkipCDO())
)
