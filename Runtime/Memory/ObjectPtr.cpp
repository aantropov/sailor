#include "ObjectPtr.hpp"
#include "Engine/Object.h"

namespace Sailor
{
	YAML::Node TObjectPtrBase::Serialize() const
	{
		YAML::Node res;
		if (m_pRawPtr)
		{
			Sailor::Serialize(res, "fileId", m_pRawPtr->GetFileId());
			Sailor::Serialize(res, "instanceId", m_pRawPtr->GetInstanceId());
		}

		return res;
	}

	bool TObjectPtrBase::IsValid() const noexcept
	{
		return m_pRawPtr != nullptr && m_pControlBlock->m_sharedPtrCounter > 0 && static_cast<Object*>(m_pRawPtr)->IsValid();
	}

	void TObjectPtrBase::ForcelyDestroyObject()
	{
		check(m_pRawPtr && m_pControlBlock);
		check(m_pControlBlock->m_sharedPtrCounter > 0);

		if (--m_pControlBlock->m_sharedPtrCounter == 0)
		{
			m_pRawPtr->~Object();
			m_pAllocator->Free(m_pRawPtr);
			m_pRawPtr = nullptr;
		}
	}
}