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
	public:

		SAILOR_API void DrawLine(const glm::vec4& start, const glm::vec4& end, const glm::vec4 color);

	protected:

		BufferPtr m_vertexBuffer;
		BufferPtr m_indexBuffer;

		CommandListPtr m_transferCmd;
		CommandListPtr m_graphicsCmd;

		SAILOR_API void Prepare();
		SAILOR_API void SubmitBuffers();

		friend class World;
	};
};