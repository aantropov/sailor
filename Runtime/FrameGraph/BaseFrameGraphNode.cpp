#include "BaseFrameGraphNode.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

void BaseFrameGraphNode::SetString(const std::string& name, const std::string& value)
{
	m_stringParams[name] = value;
}

void BaseFrameGraphNode::SetVec4(const std::string& name, const glm::vec4& value)
{
	m_vectorParams[name] = value;
}

const glm::vec4& BaseFrameGraphNode::GetVec4(const std::string& name) const
{
	return m_vectorParams[name];
}

void BaseFrameGraphNode::SetFloat(const std::string& name, float value)
{
	m_floatParams[name] = value;
}

float BaseFrameGraphNode::GetFloat(const std::string& name) const
{
	return m_floatParams[name];
}

void BaseFrameGraphNode::SetRHIResource_Unresolved(const std::string& name, const std::string& value)
{
	m_unresolvedResourceParams[name] = value;
}

void BaseFrameGraphNode::SetRHIResource(const std::string& name, RHIResourcePtr value)
{
	m_resourceParams[name] = value;
}

RHITexturePtr BaseFrameGraphNode::GetResolvedAttachment(const std::string& name) const
{
	RHI::RHITexturePtr colorAttachment;
	if (RHISurfacePtr surface = GetRHIResource(name).DynamicCast<RHISurface>())
	{
		colorAttachment = surface->GetResolved();
	}
	else if (RHITexturePtr texture = GetRHIResource(name).DynamicCast<RHITexture>())
	{
		colorAttachment = texture;
	}

	return colorAttachment;
}

RHIResourcePtr BaseFrameGraphNode::GetRHIResource(const std::string& name) const
{
	if (!m_resourceParams.ContainsKey(name))
	{
		return RHIResourcePtr();
	}

	return m_resourceParams[name];
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