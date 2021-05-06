#pragma once
#include "ExportDef.h"

template<typename T>
class Singleton
{
protected:

    static T* m_pInstance;

	SAILOR_API Singleton() = default;
    virtual SAILOR_API ~Singleton() = default;
	
public:

    static SAILOR_API T* GetInstance() { return m_pInstance; }
    static SAILOR_API void Shutdown() { delete m_pInstance; }
};

template<typename T>
T* Singleton<T>::m_pInstance = nullptr;
