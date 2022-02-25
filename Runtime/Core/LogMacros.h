#pragma once
#include "Core/SpinLock.h"
#include <string>

extern Sailor::SpinLock m_lockLog;

#define SAILOR_LOG(Format, ...) \
	{ \
	m_lockLog.Lock(); \
	printf(Format, __VA_ARGS__); \
	printf("\n"); \
	m_lockLog.Unlock(); \
	}
