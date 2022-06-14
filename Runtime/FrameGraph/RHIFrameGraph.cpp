#include "RHIFrameGraph.h"

using namespace Sailor;
using namespace Sailor::RHI;

void RHIFrameGraph::SetSampler(const std::string& name, RHI::RHITexturePtr sampler)
{
	m_samplers[name] = sampler;
}

void RHIFrameGraph::Clear()
{
	m_samplers.Clear();
	m_graph.Clear();
	m_values.Clear();
}