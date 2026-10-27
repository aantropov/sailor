#pragma once

#include "Core/Defines.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Sailor::EditorRuntime
{
	enum class EMaterialPreviewPrimitive : uint32_t { Sphere = 1, Plane = 2, Box = 3 };
	enum class EMaterialPreviewState : uint32_t { Pending = 1, Ready = 2, Error = 3, Closed = 4 };

	struct MaterialPreviewRequest
	{
		uint64_t m_sessionId = 0;
		uint64_t m_revision = 0;
		std::string m_materialYaml;
		EMaterialPreviewPrimitive m_primitive = EMaterialPreviewPrimitive::Sphere;
		float m_yaw = 0.0f;
		float m_pitch = 0.0f;
		float m_distance = 3.0f;
		uint32_t m_width = 384;
		uint32_t m_height = 384;
	};

	struct MaterialPreviewResult
	{
		uint64_t m_sessionId = 0;
		uint64_t m_revision = 0;
		EMaterialPreviewState m_state = EMaterialPreviewState::Closed;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		std::vector<uint8_t> m_png;
		std::string m_diagnostic;
	};

	SAILOR_API bool UpdateMaterialPreview(MaterialPreviewRequest request, MaterialPreviewResult& result,
		std::string& diagnostic);
	SAILOR_API MaterialPreviewResult GetMaterialPreview(uint64_t sessionId, uint64_t afterRevision = 0);
	SAILOR_API MaterialPreviewResult CloseMaterialPreview(uint64_t sessionId);
	// Called after the scheduler drains, before destroying the graphics driver.
	SAILOR_API void ResetMaterialPreviewForAppLifecycle();
	SAILOR_API uint64_t GetMaterialPreviewRenderCount();
}
