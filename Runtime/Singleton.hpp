#pragma once

template<typename T>
class Singleton
{
protected:

    static T* instance;

	Singleton() = default;
    virtual ~Singleton() = default;
	
public:

    static T* GetInstance() { return instance; }
    static void Shutdown() { delete instance; }
};

template<typename T>
T* Singleton<T>::instance = nullptr;
