#include "Mesh.h"
#include "Types.h"
#include "Buffer.h"
#include "Fence.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GfxDevice::Vulkan;

void Mesh::TraceVisit(TRefPtr<Resource> visitor, bool& bShouldRemoveFromList)
{
	bShouldRemoveFromList = false;

	if (auto fence = FencePtr(visitor.GetRawPtr()))
	{
		if (fence->IsFinished())
		{
			auto it = std::find_if(m_dependencies.begin(), m_dependencies.end(),
				[&fence](const auto& lhs)
			{
				return fence.GetRawPtr() == lhs.GetRawPtr();
			});

			if (it != std::end(m_dependencies))
			{
				std::iter_swap(it, m_dependencies.end() - 1);
				m_dependencies.pop_back();
				bShouldRemoveFromList = true;
			}
		}
	}
}

bool Mesh::IsReady() const
{
	return m_vertexBuffer->GetSize() > 0 && m_indexBuffer->GetSize() > 0 && m_dependencies.size() == 0;
}

