#include "Memory.h"
#include <cstdlib>
#include <malloc.h>

using namespace Sailor;
using namespace Sailor::Memory;

void* HeapAllocator::Allocate(size_t size)
{
	return malloc(size);
}

void HeapAllocator::Free(void* pData)
{
	free(pData);
}

