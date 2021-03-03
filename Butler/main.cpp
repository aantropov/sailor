#include <wtypes.h>
#include "Sailor.h"

using namespace Sailor;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	EngineInstance::Initialize();
	EngineInstance::Start();
	EngineInstance::Release();
	
    return 0;
}