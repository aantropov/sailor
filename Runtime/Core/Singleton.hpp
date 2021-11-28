#pragma once
#include "Defines.h"

template<typename T>
class SAILOR_API TSingleton
{
protected:

	static T* m_pInstance;

	TSingleton() = default;
	virtual ~TSingleton() = default;

	TSingleton(TSingleton&& move) = delete;
	TSingleton(TSingleton& copy) = delete;
	TSingleton& operator =(TSingleton& rhs) = delete;

public:

	static  T* GetInstance() { return m_pInstance; }
	static void Shutdown()
	{
		delete m_pInstance;
		m_pInstance = nullptr;
	}
};

#ifndef _SAILOR_IMPORT_

template<typename T>
T* TSingleton<T>::m_pInstance = nullptr;

#endif