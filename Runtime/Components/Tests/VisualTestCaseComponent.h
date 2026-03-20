#pragma once
#include "Sailor.h"
#include "Components/Tests/TestCaseComponent.h"

namespace Sailor
{
	class VisualTestCaseComponent : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(VisualTestCaseComponent)

	public:

		SAILOR_API virtual void Tick(float deltaTime) override;

		SAILOR_API const std::string& GetScreenshotName() const { return m_screenshotName; }
		SAILOR_API void SetScreenshotName(const std::string& value) { m_screenshotName = value; }

		SAILOR_API uint32_t GetCaptureAfterFrames() const { return m_captureAfterFrames; }
		SAILOR_API void SetCaptureAfterFrames(uint32_t value) { m_captureAfterFrames = value; }

	protected:

		std::string m_screenshotName = "visual-test";
		uint32_t m_captureAfterFrames = 8;

		SAILOR_API virtual const char* GetTestType() const override { return "Visual"; }

		uint32_t m_framesSinceStart = 0;
		uint32_t m_framesAfterCaptureRequest = 0;
		bool m_bCaptureRequested = false;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::VisualTestCaseComponent, bases<Sailor::TestCaseComponent>),

	func(GetScreenshotName, property("screenshotName"), SkipCDO()),
	func(SetScreenshotName, property("screenshotName"), SkipCDO()),

	func(GetCaptureAfterFrames, property("captureAfterFrames"), SkipCDO()),
	func(SetCaptureAfterFrames, property("captureAfterFrames"), SkipCDO())
)
