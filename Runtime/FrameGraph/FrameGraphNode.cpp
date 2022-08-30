#include "FrameGraphNode.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* RHINodeDefault::m_name = "untitled";
#endif

namespace Sailor::Internal
{
	TUniquePtr<TMap<std::string, std::function<FrameGraphNodePtr(void)>>> g_pNodeFactoryMethods;
}

void FrameGraphBuilder::RegisterFrameGraphNode(const std::string& nodeName, std::function<FrameGraphNodePtr(void)> factoryMethod)
{
	if (!Internal::g_pNodeFactoryMethods)
	{
		Internal::g_pNodeFactoryMethods = TUniquePtr<TMap<std::string, std::function<FrameGraphNodePtr(void)>>>::Make();
	}

	(*Internal::g_pNodeFactoryMethods)[nodeName] = factoryMethod;
}

FrameGraphNodePtr FrameGraphBuilder::CreateNode(const std::string& nodeName) const
{
	if ((*Internal::g_pNodeFactoryMethods).ContainsKey(nodeName))
	{
		return (*Internal::g_pNodeFactoryMethods)[nodeName]();
	}

	return nullptr;
}
