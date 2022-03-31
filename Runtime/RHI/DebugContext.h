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
	class DebugContext
	{
		struct LineProxy
		{
			// Locations are in world space
			glm::vec4 m_startLocation{};
			glm::vec4 m_endLocation{};

			glm::vec4 m_color{};

			// 0.0f means one frame
			float m_lifetime = -1.0f;
		};

	public:

		SAILOR_API void DrawLine(const glm::vec4& start, const glm::vec4& end, const glm::vec4 color = {0.0f, 1.0f, 0.0f, 0.0f}, float duration = 0.0f);
		SAILOR_API void RenderAll(float deltaTime);

	protected:

		TVector<LineProxy> m_lines;

		MeshPtr m_mesh;
		MaterialPtr m_material;

		CommandListPtr m_transferCmd;
		CommandListPtr m_graphicsCmd;

		friend class World;
	};
};