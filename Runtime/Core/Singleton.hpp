#pragma once
#include "Defines.h"

template<typename T>
class TSingleton
{
protected:

	static T* m_pInstance;

	SAILOR_API TSingleton() = default;
	virtual SAILOR_API ~TSingleton() = default;

	SAILOR_API TSingleton(TSingleton&& move) = delete;
	SAILOR_API TSingleton(TSingleton& copy) = delete;
	SAILOR_API TSingleton& operator =(TSingleton& rhs) = delete;
	
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
