#pragma once
#include "Sailor.h"
#include "Components/Tests/TestCaseComponent.h"

namespace Sailor
{
	class PathTracerTestCaseComponent : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(PathTracerTestCaseComponent)

	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;

		SAILOR_API const std::string& GetOutputName() const { return m_outputName; }
		SAILOR_API void SetOutputName(const std::string& value) { m_outputName = value; }

		SAILOR_API uint32_t GetCaptureAfterFrames() const { return m_captureAfterFrames; }
		SAILOR_API void SetCaptureAfterFrames(uint32_t value) { m_captureAfterFrames = value; }

		SAILOR_API uint32_t GetTimeoutFrames() const { return m_timeoutFrames; }
		SAILOR_API void SetTimeoutFrames(uint32_t value) { m_timeoutFrames = value; }

		SAILOR_API bool GetEnableNode() const { return m_bEnableNode; }
		SAILOR_API void SetEnableNode(bool value) { m_bEnableNode = value; }
		SAILOR_API bool GetAttachToAllMeshes() const { return m_bAttachToAllMeshes; }
		SAILOR_API void SetAttachToAllMeshes(bool value) { m_bAttachToAllMeshes = value; }
		SAILOR_API bool GetRebuildEveryFrame() const { return m_bRebuildEveryFrame; }
		SAILOR_API void SetRebuildEveryFrame(bool value) { m_bRebuildEveryFrame = value; }
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

		SAILOR_API bool AttachProxies();
		SAILOR_API bool ConfigurePathTracer();
		SAILOR_API bool CaptureOutput(std::string& outError) const;
		SAILOR_API virtual const char* GetTestType() const override { return "PathTracer"; }

		std::string m_outputName = "path-trace";
		uint32_t m_captureAfterFrames = 240;
		uint32_t m_timeoutFrames = 240;
		bool m_bEnableNode = true;
		bool m_bAttachToAllMeshes = false;
		bool m_bRebuildEveryFrame = false;
		float m_samplesPerFrame = 8.0f;
		float m_maxBounces = 3.0f;
		float m_blend = 1.0f;
		float m_rayBiasBase = 0.05f;
		float m_rayBiasScale = 1.0f;
		bool m_bPendingPathTracerConfig = false;
		bool m_bProxiesAttached = false;
		bool m_bCaptureRequested = false;
		uint32_t m_framesSinceStart = 0;
		uint32_t m_framesAfterCaptureRequest = 0;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::PathTracerTestCaseComponent, bases<Sailor::TestCaseComponent>),

	func(GetOutputName, property("outputName"), SkipCDO()),
	func(SetOutputName, property("outputName"), SkipCDO()),
	func(GetCaptureAfterFrames, property("captureAfterFrames"), SkipCDO()),
	func(SetCaptureAfterFrames, property("captureAfterFrames"), SkipCDO()),
	func(GetTimeoutFrames, property("timeoutFrames"), SkipCDO()),
	func(SetTimeoutFrames, property("timeoutFrames"), SkipCDO()),
	func(GetEnableNode, property("enableNode"), SkipCDO()),
	func(SetEnableNode, property("enableNode"), SkipCDO()),
	func(GetAttachToAllMeshes, property("attachToAllMeshes"), SkipCDO()),
	func(SetAttachToAllMeshes, property("attachToAllMeshes"), SkipCDO()),
	func(GetRebuildEveryFrame, property("rebuildEveryFrame"), SkipCDO()),
	func(SetRebuildEveryFrame, property("rebuildEveryFrame"), SkipCDO()),
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
