#include "BaseFrameGraphNode.h"
#include "RHIFrameGraph.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"
#include <format>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

void BaseFrameGraphNode::SetString(StringHash name, std::string_view value)
{
	m_stringParams[name] = value;
	++m_parameterRevision;
	if (name == "Tag"_h || name == "shader"_h) m_gpuTimingName = {};
}

StringHash BaseFrameGraphNode::GetGpuTimingName(size_t nodeIndex)
{
	if (m_gpuTimingName.IsEmpty() || m_gpuTimingNodeIndex != nodeIndex)
	{
		std::string name = std::format("{:02d} {}", nodeIndex, m_tag.ToString());
		for (const auto parameter : { "Tag"_h, "shader"_h })
		{
			std::string_view value;
			if (TryGetString(parameter, value) && !value.empty())
			{
				name += '/';
				name += value;
			}
		}
		m_gpuTimingName = StringHash::Runtime(name);
		m_gpuTimingNodeIndex = nodeIndex;
	}
	return m_gpuTimingName;
}

void BaseFrameGraphNode::SetVec4(StringHash name, const glm::vec4& value)
{
	m_vectorParams[name] = value;
	++m_parameterRevision;
}

const glm::vec4& BaseFrameGraphNode::GetVec4(StringHash name) const
{
	return m_vectorParams[name];
}

void BaseFrameGraphNode::SetFloat(StringHash name, float value)
{
	m_floatParams[name] = value;
	++m_parameterRevision;
}

float BaseFrameGraphNode::GetFloat(StringHash name) const
{
	return m_floatParams[name];
}

void BaseFrameGraphNode::SetRHIResource_Unresolved(StringHash name, StringHash value)
{
	m_unresolvedResourceParams[name] = value;
	m_resourceParams.Remove(name);
	++m_parameterRevision;
	++m_resourceRevision;
}

void BaseFrameGraphNode::SetRHIResource(StringHash name, RHIResourcePtr value)
{
	m_resourceParams[name] = value;
	m_unresolvedResourceParams.Remove(name);
	++m_parameterRevision;
	++m_resourceRevision;
}

RHITexturePtr BaseFrameGraphNode::GetResolvedAttachment(StringHash name, const RHIFrameGraph* frameGraph) const
{
	auto resource = GetRHIResource(name, frameGraph);
	if (const auto surface = resource.DynamicCast<RHISurface>())
	{
		return surface->GetResolved();
	}
	return resource.DynamicCast<RHITexture>();
}

RHITexturePtr BaseFrameGraphNode::GetTargetAttachment(StringHash name, const RHIFrameGraph* frameGraph) const
{
	auto resource = GetRHIResource(name, frameGraph);
	if (const auto surface = resource.DynamicCast<RHISurface>())
	{
		return surface->GetTarget();
	}
	return resource.DynamicCast<RHITexture>();
}

RHITexturePtr BaseFrameGraphNode::GetSampledAttachment(StringHash name, const RHIFrameGraph* frameGraph) const
{
	auto texture = GetResolvedAttachment(name, frameGraph);
	if (const auto target = texture.DynamicCast<RHIRenderTarget>())
	{
		if (const auto depth = target->GetDepthAspect()) return depth;
	}
	return texture;
}

RHIResourcePtr BaseFrameGraphNode::GetRHIResource(StringHash name, const RHIFrameGraph* frameGraph) const
{
	const RHIResourcePtr* resource = nullptr;
	if (m_resourceParams.Find(name, resource)) return frameGraph ? frameGraph->ResolveResource(*resource) : *resource;
	const StringHash* resourceName = nullptr;
	if (frameGraph && m_unresolvedResourceParams.Find(name, resourceName))
	{
		return frameGraph->GetResource(*resourceName);
	}
	return {};
}

const std::string& BaseFrameGraphNode::GetString(StringHash name) const
{
	return m_stringParams[name];
}

bool BaseFrameGraphNode::TryGetString(StringHash name, std::string& value) const
{
	std::string_view view;
	if (!TryGetString(name, view)) return false;
	value = view;
	return true;
}

bool BaseFrameGraphNode::TryGetString(StringHash name, std::string_view& value) const
{
	const std::string* stored = nullptr;
	if (!m_stringParams.Find(name, stored)) return false;
	value = *stored;
	return true;
}
