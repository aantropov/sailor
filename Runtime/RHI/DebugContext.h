#pragma once
#include <functional>
#include <atomic>
#include <thread>
#include <glm/glm/glm.hpp>

#include "Core/Defines.h"
#include "Types.h"
#include "Memory/RefPtr.hpp"
#include "Memory/UniquePtr.hpp"
#include "Core/Submodule.h"
#include "Jobsystem/JobSystem.h"
#include "GraphicsDriver.h"
#include "Renderer.h"
#include "Buffer.h"

namespace Sailor
{
	class World;
}

namespace Sailor::RHI
{
	class DebugFrame
	{
	public:
		RHI::CommandListPtr m_drawDebugMeshCmd{};
		RHI::SemaphorePtr m_signalSemaphore{};
	};

	class DebugContext
	{
		struct LineProxy
		{
			// Locations are in world space
			glm::vec3 m_startLocation{};
			glm::vec3 m_endLocation{};

			glm::vec4 m_color{};

			// 0.0f means one frame
			float m_lifetime = -1.0f;
		};

	public:

		SAILOR_API __forceinline void DrawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec4 color = { 0.0f, 1.0f, 0.0f, 0.0f }, float duration = 0.0f);
		SAILOR_API void DrawOrigin(const glm::vec3& position, float size = 1.0f, float duration = 0.0f);
		SAILOR_API void DrawAABB(const Math::AABB& aabb, const glm::vec4 color = { 0.0f, 0.0f, 1.0f, 0.0f }, float duration = 0.0f);
		SAILOR_API void DrawSphere(const glm::vec3& position, float size = 1.0f, const glm::vec4 color = { 0.0f, 0.0f, 0.0f, 1.0f }, float duration = 0.0f);

		// TODO: Split logic and rendering
		SAILOR_API DebugFrame Tick(RHI::ShaderBindingSetPtr frameBindings, float deltaTime);

	protected:

		SAILOR_API RHI::CommandListPtr CreateRenderingCommandList(RHI::ShaderBindingSetPtr frameBindings, RHI::MeshPtr debugMesh) const;

		TVector<LineProxy> m_lines;
		MaterialPtr m_material;

		friend class World;
	};
};