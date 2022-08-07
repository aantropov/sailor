#pragma once
#include <functional>
#include <atomic>
#include <thread>
#include <glm/glm/glm.hpp>

#include "Core/Defines.h"
#include "Types.h"
#include "Math/Bounds.h"
#include "Memory/RefPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "Core/Submodule.h"
#include "Tasks/Scheduler.h"
#include "GraphicsDriver.h"
#include "Buffer.h"

namespace Sailor
{
	class World;
}

namespace Sailor::RHI
{
	class DebugContext
	{
	public:

		SAILOR_API __forceinline void DrawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec4 color = { 0.0f, 1.0f, 0.0f, 0.0f }, float duration = 0.0f);
		SAILOR_API void DrawOrigin(const glm::vec3& position, float size = 1.0f, float duration = 0.0f);
		SAILOR_API void DrawAABB(const Math::AABB& aabb, const glm::vec4 color = { 0.0f, 0.0f, 1.0f, 0.0f }, float duration = 0.0f);
		SAILOR_API void DrawSphere(const glm::vec3& position, float size = 1.0f, const glm::vec4 color = { 0.0f, 0.0f, 0.0f, 1.0f }, float duration = 0.0f);
		SAILOR_API void DrawFrustum(const glm::mat4& worldMatrix, float fovDegrees, float maxRange, float minRange, float aspect, const glm::vec4 color = { 0.0f, 1.0f, 0.3f, 0.0f }, float duration = 0.0f);

		SAILOR_API void Tick(RHI::RHICommandListPtr cmdList, float deltaTime);		
		SAILOR_API void DrawDebugMesh(RHI::RHICommandListPtr drawCmdList, RHI::RHIShaderBindingSetPtr frameBindings) const;

	protected:

		SAILOR_API void UpdateDebugMesh(RHI::RHICommandListPtr transferCmdList);

		bool m_bShouldUpdateMeshThisFrame = false;
		TVector<uint32_t> m_cachedIndices{};
		RHI::RHIMeshPtr m_cachedMesh{};

		TVector<VertexP3C4> m_lineVertices{};
		TVector<float> m_lifetimes;
		int32_t m_lineVerticesOffset = -1;

		RHIMaterialPtr m_material{};
	};
};