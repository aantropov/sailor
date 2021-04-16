#pragma once
#include "ExportDef.h"

template<typename T>
class Singleton
{
protected:

    static T* instance;

	SAILOR_API Singleton() = default;
    virtual SAILOR_API ~Singleton() = default;
	
public:

    __forceinline static SAILOR_API T* GetInstance() { return instance; }
    static SAILOR_API void Shutdown() { delete instance; }
};

template<typename T>
T* Singleton<T>::instance = nullptr;
