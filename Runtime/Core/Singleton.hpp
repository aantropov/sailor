#pragma once
#include "Defines.h"

template<typename T>
class SAILOR_API TSingleton
{
protected:

	static T* s_pInstance;

	TSingleton() = default;
	virtual ~TSingleton() = default;

	TSingleton(TSingleton&& move) = delete;
	TSingleton(TSingleton& copy) = delete;
	TSingleton& operator =(TSingleton& rhs) = delete;

public:

	static  T* GetInstance() { return s_pInstance; }
	static void Shutdown()
	{
		delete s_pInstance;
		s_pInstance = nullptr;
	}
};

#ifndef _SAILOR_IMPORT_

template<typename T>
T* TSingleton<T>::s_pInstance = nullptr;

#endif