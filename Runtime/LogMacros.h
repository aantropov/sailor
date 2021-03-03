#pragma once
#include <string>

#define SAILOR_LOG(Format, ...) \
	{ \
	printf(Format, __VA_ARGS__); \
	printf("\n"); \
	} 
