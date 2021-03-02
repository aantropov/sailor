#include <wtypes.h>
#include "Sailor.h"

using namespace Sailor;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	GEngineInstance::Initialize();
	GEngineInstance::Run();
	GEngineInstance::Stop();
	
    return 0;
}