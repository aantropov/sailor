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
#include "Containers/Octree.h"

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
		uint32_t m_linesCount{};
	};

	class DebugContext
	{

	public:

		SAILOR_API __forceinline void DrawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec4 color = { 0.0f, 1.0f, 0.0f, 0.0f }, float duration = 0.0f);
		SAILOR_API void DrawOrigin(const glm::vec3& position, float size = 1.0f, float duration = 0.0f);
		SAILOR_API void DrawAABB(const Math::AABB& aabb, const glm::vec4 color = { 0.0f, 0.0f, 1.0f, 0.0f }, float duration = 0.0f);
		SAILOR_API void DrawSphere(const glm::vec3& position, float size = 1.0f, const glm::vec4 color = { 0.0f, 0.0f, 0.0f, 1.0f }, float duration = 0.0f);

		// TODO: Split logic and rendering
		SAILOR_API DebugFrame Tick(RHI::ShaderBindingSetPtr frameBindings, float deltaTime);

	protected:

		/*
		template<typename T>
		SAILOR_API void DrawOctree(const TOctree<T>& octree, float duration = 0.0f)
		{
			DrawOctree_Internal(octree.m_root, duration);
		}

		template<typename T>
		SAILOR_API void DrawOctree_Internal(const TOctree<T>::TNode& node, float duration = 0.0f)
		{
			if (!node->IsLeaf())
			{
				for (uint32_t i = 0; i < 8; i++)
				{
					DrawOctree_Internal(node.m_internal[i], duration))
				}
			}

			Math::AABB aabb;
			aabb.m_min = node.m_center - node.m_size * glm::vec3(0.5f, 0.5f, 0.5f);
			aabb.m_min = node.m_center + node.m_size * glm::vec3(0.5f, 0.5f, 0.5f);

			DrawAABB(aabb, duration);
		}*/

		SAILOR_API RHI::CommandListPtr CreateRenderingCommandList(RHI::ShaderBindingSetPtr frameBindings, RHI::MeshPtr debugMesh) const;

		TVector<uint32_t> m_cachedIndices{};
		RHI::MeshPtr m_cachedMesh{};
		DebugFrame m_cachedFrame{};

		TVector<VertexP3C4> m_lineVertices{};
		TVector<float> m_lifetimes;
		int32_t m_lineVerticesOffset = -1;

		MaterialPtr m_material{};

		friend class World;
	};
};