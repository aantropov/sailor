#include "BaseFrameGraphNode.h"

using namespace Sailor;
using namespace Sailor::RHI;

void IBaseFrameGraphNode::SetVectorParam(std::string name, const glm::vec4& value)
{
	m_vectorParams[name] = value;
}

glm::vec4 IBaseFrameGraphNode::GetVectorParam(std::string name) const
{
	return m_vectorParams[name];
}

void IBaseFrameGraphNode::SetResourceParam(std::string name, RHIResourcePtr value)
{
	m_resourceParams[name] = value;
}

RHIResourcePtr IBaseFrameGraphNode::GetResourceParam(std::string name) const
{
	return m_resourceParams[name];
}
