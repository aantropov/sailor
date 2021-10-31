#pragma once
#include "Core/RefPtr.hpp"
#include "Buffer.h"
#include "Types.h"

using namespace GfxDevice::Vulkan;

namespace Sailor::RHI
{
	class Mesh : public Resource, public IObservable, public IDependent
	{
	public:

		TRefPtr<Buffer> m_vertexBuffer;
		TRefPtr<Buffer> m_indexBuffer;
		
		virtual void TraceVisit(class TRefPtr<Resource> visitor, bool& bShouldRemoveFromList) override;

		bool IsReady() const;
	protected:

	};
};
