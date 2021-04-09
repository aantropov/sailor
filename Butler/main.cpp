#include <wtypes.h>
#include "Sailor.h"
#include "AssetRegistry/ShaderCompiler.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	EngineInstance::Initialize();

	auto assetRegistry = AssetRegistry::GetInstance();

	if (auto shaderUID = assetRegistry->GetAssetInfo("Shaders\\Simple.shader"))
	{
		GfxDeviceVulkan::ShaderCompiler::CreatePrecompiledShaders(shaderUID->GetUID());
	}

	EngineInstance::Start();
	EngineInstance::Shutdown();
		
    return 0;
}
