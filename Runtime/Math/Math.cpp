#include "Math.h"

using namespace Sailor;
using namespace Sailor::Math;

unsigned long Sailor::Math::UpperPowOf2(unsigned long v)
{
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	v++;
	return v;
}
