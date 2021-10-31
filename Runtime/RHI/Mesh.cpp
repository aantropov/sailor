#include "Mesh.h"
#include "Types.h"
#include "Fence.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GfxDevice::Vulkan;

void Mesh::TraceVisit(IVisitor& visitor, bool& bShouldRemoveFromList)
{
	bShouldRemoveFromList = false;

	if (auto fence = dynamic_cast<TRefPtr<RHI::Fence>*>(&visitor))
	{
		if ((*fence)->IsFinished())
		{
		/*	auto res = TRefPtr<Resource>(dynamic_cast<Resource*>(&visitor));
			auto it = std::find(m_dependencies.begin(), m_dependencies.end(), res);
			if (it != std::end(m_dependencies))
			{
				std::iter_swap(it, m_dependencies.end() - 1);
				m_dependencies.pop_back();
				bShouldRemoveFromList = true;
			}*/
		}
	}
}

bool Mesh::IsReady() const
{
	for (auto& dep : m_dependencies)
	{
		/*if (auto fence = dynamic_cast<RHI::Fence*>(dep.GetRawPtr()))
		{
			if (!fence->IsFinished())
			{
				return false;
			}
		}*/
	}

	return m_vertexBuffer->GetSize() > 0 && m_indexBuffer->GetSize() > 0;
}

