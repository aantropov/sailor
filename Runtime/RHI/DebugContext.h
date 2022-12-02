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
#include "Core/SpinLock.h"

namespace Sailor
{
	class World;
}

namespace Sailor::RHI
{
	class DebugContext
	{
	public:

		static constexpr glm::vec4 Color_Inactive = glm::vec4(0.66f, 0.66f, 0.66f, 0.66f);
		static constexpr glm::vec4 Color_CmdTransfer = glm::vec4(0.85f, 0.85f, 1.0f, 0.85f);
		static constexpr glm::vec4 Color_CmdCompute = glm::vec4(1.0f, 0.65f, 0.0f, 0.25f);
		static constexpr glm::vec4 Color_CmdGraphics = glm::vec4(0.0f, 1.0f, 0.0f, 0.25f);
		static constexpr glm::vec4 Color_CmdPostProcess = glm::vec4(1.0f, 0.65f, 0.5f, 0.35f);
		static constexpr glm::vec4 Color_CmdDebug = glm::vec4(1.0f, 1.0f, 0.0f, 0.25f);

		SAILOR_API __forceinline void DrawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec4 color = { 0.0f, 1.0f, 0.0f, 0.0f }, float duration = 0.0f);
		SAILOR_API void DrawOrigin(const glm::vec3& position, const glm::mat4& origin, float size = 1.0f, float duration = 0.0f);
		SAILOR_API void DrawArrow(const glm::vec3& start, const glm::vec3& end, const glm::vec4 color = { 0.0f, 1.0f, 0.0f, 0.0f }, float duration = 0.0f);
		SAILOR_API void DrawAABB(const Math::AABB& aabb, const glm::vec4 color = { 0.0f, 0.0f, 1.0f, 0.0f }, float duration = 0.0f);
		SAILOR_API void DrawSphere(const glm::vec3& position, float radius = 1.0f, const glm::vec4 color = { 0.0f, 0.0f, 0.0f, 1.0f }, float duration = 0.0f);
		SAILOR_API void DrawCone(const glm::vec3& start, const glm::vec3& end, float degrees, const glm::vec4 color = { 0.0f, 0.0f, 0.0f, 1.0f }, float duration = 0.0f);
		SAILOR_API void DrawFrustum(const glm::mat4& worldMatrix, float fovDegrees, float maxRange, float minRange, float aspect, const glm::vec4 color = { 0.0f, 1.0f, 0.3f, 0.0f }, float duration = 0.0f);
		SAILOR_API void DrawPlane(const Math::Plane& plane, float size, const glm::vec4 color = { 0.0f, 1.0f, 0.3f, 0.0f }, float duration = 0.0f);
		SAILOR_API void Tick(RHI::RHICommandListPtr transferCmd, float deltaTime);
		SAILOR_API void DrawDebugMesh(RHI::RHICommandListPtr secondaryDrawCmdList, const glm::mat4x4& viewProjection) const;

	protected:

		SAILOR_API void UpdateDebugMesh(RHI::RHICommandListPtr transferCmdList);

		bool m_bShouldUpdateMeshThisFrame = false;
		TVector<uint32_t> m_cachedIndices{};
		RHI::RHIMeshPtr m_cachedMesh{};

		TVector<VertexP3C4> m_lineVertices{};
		TVector<float> m_lifetimes;
		int32_t m_lineVerticesOffset = -1;
		uint32_t m_numRenderedVertices = 0;

		RHIMaterialPtr m_material{};
	};
};