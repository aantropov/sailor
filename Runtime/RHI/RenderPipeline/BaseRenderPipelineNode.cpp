#include "BaseRenderPipelineNode.h"

using namespace Sailor;
using namespace Sailor::RHI;

void IBaseRenderPipelineNode::SetVectorParam(std::string name, const glm::vec4& value)
{
	m_vectorParams[name] = value;
}

glm::vec4 IBaseRenderPipelineNode::GetVectorParam(std::string name) const
{
	return m_vectorParams[name];
}

void IBaseRenderPipelineNode::SetResourceParam(std::string name, RHIResourcePtr value)
{
	m_resourceParams[name] = value;
}

RHIResourcePtr IBaseRenderPipelineNode::GetResourceParam(std::string name) const
{
	return m_resourceParams[name];
}
