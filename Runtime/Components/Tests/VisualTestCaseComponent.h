#pragma once
#include "Sailor.h"
#include "Components/Tests/TestCaseComponent.h"

namespace Sailor
{
	class VisualTestCaseComponent : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(VisualTestCaseComponent)

	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;

		SAILOR_API const std::string& GetScreenshotName() const { return m_screenshotName; }
		SAILOR_API void SetScreenshotName(const std::string& value) { m_screenshotName = value; }

		SAILOR_API uint32_t GetCaptureAfterFrames() const { return m_captureAfterFrames; }
		SAILOR_API void SetCaptureAfterFrames(uint32_t value) { m_captureAfterFrames = value; }
		SAILOR_API float GetMinRunTimeSeconds() const { return m_minRunTimeSeconds; }
		SAILOR_API void SetMinRunTimeSeconds(float value) { m_minRunTimeSeconds = value; }

		SAILOR_API bool GetEnablePathTracer() const { return m_bEnablePathTracer; }
		SAILOR_API void SetEnablePathTracer(bool value) { m_bEnablePathTracer = value; }
		SAILOR_API bool GetEnablePathTracerNode() const { return m_bEnablePathTracerNode; }
		SAILOR_API void SetEnablePathTracerNode(bool value) { m_bEnablePathTracerNode = value; }
		SAILOR_API float GetSamplesPerFrame() const { return m_samplesPerFrame; }
		SAILOR_API void SetSamplesPerFrame(float value) { m_samplesPerFrame = value; }
		SAILOR_API float GetMaxBounces() const { return m_maxBounces; }
		SAILOR_API void SetMaxBounces(float value) { m_maxBounces = value; }
		SAILOR_API float GetBlend() const { return m_blend; }
		SAILOR_API void SetBlend(float value) { m_blend = value; }
		SAILOR_API float GetRayBiasBase() const { return m_rayBiasBase; }
		SAILOR_API void SetRayBiasBase(float value) { m_rayBiasBase = value; }
		SAILOR_API float GetRayBiasScale() const { return m_rayBiasScale; }
		SAILOR_API void SetRayBiasScale(float value) { m_rayBiasScale = value; }

	protected:
		SAILOR_API bool ConfigurePathTracer();

		std::string m_screenshotName = "visual-test";
		uint32_t m_captureAfterFrames = 8;
		float m_minRunTimeSeconds = 0.0f;
		bool m_bEnablePathTracer = false;
		bool m_bEnablePathTracerNode = true;
		float m_samplesPerFrame = 1.0f;
		float m_maxBounces = 1.0f;
		float m_blend = 1.0f;
		float m_rayBiasBase = 0.05f;
		float m_rayBiasScale = 1.0f;
		bool m_bPendingPathTracerConfig = false;

		SAILOR_API virtual const char* GetTestType() const override { return "Visual"; }

		uint32_t m_framesSinceStart = 0;
		uint32_t m_framesAfterCaptureRequest = 0;
		int64_t m_startTimeMs = 0;
		bool m_bCaptureRequested = false;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::VisualTestCaseComponent, bases<Sailor::TestCaseComponent>),

	func(GetScreenshotName, property("screenshotName"), SkipCDO()),
	func(SetScreenshotName, property("screenshotName"), SkipCDO()),

	func(GetCaptureAfterFrames, property("captureAfterFrames"), SkipCDO()),
	func(SetCaptureAfterFrames, property("captureAfterFrames"), SkipCDO()),
	func(GetMinRunTimeSeconds, property("minRunTimeSeconds"), SkipCDO()),
	func(SetMinRunTimeSeconds, property("minRunTimeSeconds"), SkipCDO()),
	func(GetEnablePathTracer, property("enablePathTracer"), SkipCDO()),
	func(SetEnablePathTracer, property("enablePathTracer"), SkipCDO()),
	func(GetEnablePathTracerNode, property("enablePathTracerNode"), SkipCDO()),
	func(SetEnablePathTracerNode, property("enablePathTracerNode"), SkipCDO()),
	func(GetSamplesPerFrame, property("samplesPerFrame"), SkipCDO()),
	func(SetSamplesPerFrame, property("samplesPerFrame"), SkipCDO()),
	func(GetMaxBounces, property("maxBounces"), SkipCDO()),
	func(SetMaxBounces, property("maxBounces"), SkipCDO()),
	func(GetBlend, property("blend"), SkipCDO()),
	func(SetBlend, property("blend"), SkipCDO()),
	func(GetRayBiasBase, property("rayBiasBase"), SkipCDO()),
	func(SetRayBiasBase, property("rayBiasBase"), SkipCDO()),
	func(GetRayBiasScale, property("rayBiasScale"), SkipCDO()),
	func(SetRayBiasScale, property("rayBiasScale"), SkipCDO())
)
