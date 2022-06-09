#include "RenderPipelineNode.h"

using namespace Sailor;
using namespace Sailor::RHI;

void RHIRenderPipelineNode::SetVectorParam(std::string name, const glm::vec4& value)
{
	m_vectorParams[name] = value;
}

glm::vec4 RHIRenderPipelineNode::GetVectorParam(std::string name) const
{
	return m_vectorParams[name];
}

void RHIRenderPipelineNode::SetResourceParam(std::string name, RHIResourcePtr value)
{
	m_resourceParams[name] = value;
}

RHIResourcePtr RHIRenderPipelineNode::GetResourceParam(std::string name) const
{
	return m_resourceParams[name];
}
