#include "Memory.h"
#include <cstdlib>
#include <malloc.h>

using namespace Sailor;
using namespace Sailor::Memory;

void* HeapAllocator::Allocate(size_t size, HeapAllocator*)
{
	return malloc(size);
}

void HeapAllocator::Free(void* pData, HeapAllocator*)
{
	free(pData);
}

