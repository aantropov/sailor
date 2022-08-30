#include "BaseFrameGraphNode.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

void BaseFrameGraphNode::SetStringParam(const std::string& name, const std::string& value)
{
	m_stringParams[name] = value;
}

void BaseFrameGraphNode::SetVectorParam(const std::string& name, const glm::vec4& value)
{
	m_vectorParams[name] = value;
}

const glm::vec4& BaseFrameGraphNode::GetVectorParam(const std::string& name) const
{
	return m_vectorParams[name];
}

void BaseFrameGraphNode::SetResourceParam(const std::string& name, RHIResourcePtr value)
{
	m_resourceParams[name] = value;
}

RHIResourcePtr BaseFrameGraphNode::GetResourceParam(const std::string& name) const
{
	if (!m_resourceParams.ContainsKey(name))
	{
		return RHIResourcePtr();
	}
	return m_resourceParams[name];
}

const std::string& BaseFrameGraphNode::GetStringParam(const std::string& name) const
{
	return m_stringParams[name];
}
