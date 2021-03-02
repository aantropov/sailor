#include <wtypes.h>
#include "Sailor.h"

using namespace Sailor;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	GEngineInstance::Initialize();
	GEngineInstance::Start();
	GEngineInstance::Release();
	
    return 0;
}