#include "RHIDepthPrepassNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* RHIDepthPrepassNode::m_name = "DepthPrepass";
#endif

RHI::ESortingOrder RHIDepthPrepassNode::GetSortingOrder() const
{
	const std::string& sortOrder = GetStringParam("Sorting");

	if (!sortOrder.empty())
	{
		return magic_enum::enum_cast<RHI::ESortingOrder>(sortOrder).value_or(RHI::ESortingOrder::FrontToBack);
	}

	return RHI::ESortingOrder::FrontToBack;
}

struct PerInstanceData
{
	alignas(16) glm::mat4 model;

	bool operator==(const PerInstanceData& rhs) const { return this->model == model; }

	size_t GetHash() const
	{
		hash<glm::mat4> p;
		return p(model);
	}
};

class Batch
{
public:

	RHIMeshPtr m_mesh;

	Batch() = default;
	Batch(const RHIMeshPtr& mesh) : m_mesh(mesh) {}

	bool operator==(const Batch& rhs) const
	{
		return	m_mesh->m_vertexBuffer->GetCompatibilityHashCode() == rhs.m_mesh->m_vertexBuffer->GetCompatibilityHashCode() &&
			m_mesh->m_indexBuffer->GetCompatibilityHashCode() == rhs.m_mesh->m_indexBuffer->GetCompatibilityHashCode();
	}

	size_t GetHash() const
	{
		size_t hash = 0;
		HashCombine(hash, m_mesh->m_vertexBuffer->GetCompatibilityHashCode(), m_mesh->m_indexBuffer->GetCompatibilityHashCode());
		return hash;
	}
};

void RHIDepthPrepassNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
}

void RHIDepthPrepassNode::Clear()
{
}