struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#include <wtypes.h>
#include <iostream>

#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int32_t)
{
	Sailor::App::Initialize();

	App::Start();
	App::Stop();

	Sailor::App::Shutdown();

	return 0;
}
