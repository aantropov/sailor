#include "BaseFrameGraphNode.h"
#include "RHIFrameGraph.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

void BaseFrameGraphNode::SetString(const std::string& name, const std::string& value)
{
	m_stringParams[name] = value;
	++m_parameterRevision;
}

void BaseFrameGraphNode::SetVec4(const std::string& name, const glm::vec4& value)
{
	m_vectorParams[name] = value;
	++m_parameterRevision;
}

const glm::vec4& BaseFrameGraphNode::GetVec4(const std::string& name) const
{
	return m_vectorParams[name];
}

void BaseFrameGraphNode::SetFloat(const std::string& name, float value)
{
	m_floatParams[name] = value;
	++m_parameterRevision;
}

float BaseFrameGraphNode::GetFloat(const std::string& name) const
{
	return m_floatParams[name];
}

void BaseFrameGraphNode::SetRHIResource_Unresolved(const std::string& name, const std::string& value)
{
	m_unresolvedResourceParams[name] = value;
	m_resourceParams.Remove(name);
	++m_parameterRevision;
}

void BaseFrameGraphNode::SetRHIResource(const std::string& name, RHIResourcePtr value)
{
	m_resourceParams[name] = value;
	m_unresolvedResourceParams.Remove(name);
	++m_parameterRevision;
}

RHITexturePtr BaseFrameGraphNode::GetResolvedAttachment(const std::string& name, const RHIFrameGraph* frameGraph) const
{
	auto resource = GetRHIResource(name, frameGraph);
	if (const auto surface = resource.DynamicCast<RHISurface>())
	{
		return surface->GetResolved();
	}
	return resource.DynamicCast<RHITexture>();
}

RHITexturePtr BaseFrameGraphNode::GetTargetAttachment(const std::string& name, const RHIFrameGraph* frameGraph) const
{
	auto resource = GetRHIResource(name, frameGraph);
	if (const auto surface = resource.DynamicCast<RHISurface>())
	{
		return surface->GetTarget();
	}
	return resource.DynamicCast<RHITexture>();
}

RHITexturePtr BaseFrameGraphNode::GetSampledAttachment(const std::string& name, const RHIFrameGraph* frameGraph) const
{
	auto texture = GetResolvedAttachment(name, frameGraph);
	if (const auto target = texture.DynamicCast<RHIRenderTarget>())
	{
		if (const auto depth = target->GetDepthAspect()) return depth;
	}
	return texture;
}

RHIResourcePtr BaseFrameGraphNode::GetRHIResource(const std::string& name, const RHIFrameGraph* frameGraph) const
{
	const RHIResourcePtr* resource = nullptr;
	if (m_resourceParams.Find(name, resource)) return frameGraph ? frameGraph->ResolveResource(*resource) : *resource;
	const std::string* resourceName = nullptr;
	if (frameGraph && m_unresolvedResourceParams.Find(name, resourceName))
	{
		return frameGraph->GetResource(*resourceName);
	}
	return {};
}

const std::string& BaseFrameGraphNode::GetString(const std::string& name) const
{
	return m_stringParams[name];
}

bool BaseFrameGraphNode::TryGetString(const std::string& name, string& string) const
{
	if (m_stringParams.ContainsKey(name))
	{
		string = m_stringParams[name];
		return true;
	}

	return false;
}
