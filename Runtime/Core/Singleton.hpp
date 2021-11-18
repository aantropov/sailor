#pragma once
#include "Defines.h"

template<typename T>
class TSingleton
{
protected:

	static T* m_pInstance;

	SAILOR_API TSingleton() = default;
	virtual SAILOR_API ~TSingleton() = default;

	TSingleton(TSingleton&& move) = delete;
	TSingleton(TSingleton& copy) = delete;
	TSingleton& operator =(TSingleton& rhs) = delete;
	
public:

	static SAILOR_API T* GetInstance() { return m_pInstance; }
	static SAILOR_API void Shutdown() 
	{
		delete m_pInstance;
		m_pInstance = nullptr;
	}
};

template<typename T>
T* TSingleton<T>::m_pInstance = nullptr;
