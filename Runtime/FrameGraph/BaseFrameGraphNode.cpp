#include "BaseFrameGraphNode.h"

using namespace Sailor;
using namespace Sailor::RHI;

void BaseFrameGraphNode::SetVectorParam(std::string name, const glm::vec4& value)
{
	m_vectorParams[name] = value;
}

glm::vec4 BaseFrameGraphNode::GetVectorParam(std::string name) const
{
	return m_vectorParams[name];
}

void BaseFrameGraphNode::SetResourceParam(std::string name, RHIResourcePtr value)
{
	m_resourceParams[name] = value;
}

RHIResourcePtr BaseFrameGraphNode::GetResourceParam(std::string name) const
{
	return m_resourceParams[name];
}
