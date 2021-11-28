struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#include <wtypes.h>
#include <iostream>

#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int32_t)
{
	Sailor::App::Initialize();

	//auto assetRegistry = App::GetSubmodule<AssetRegistry>();
	/*auto shaderCompiler = ShaderCompiler::GetInstance();

	if (auto shaderUID = assetRegistry->GetAssetInfoPtr("Shaders\\Simple.shader"))
	{
		ShaderCompiler::GetInstance()->CompileAllPermutations(shaderUID->GetUID());

		//ShaderCompiler::GetInstance()->GetSpirvCode(shaderUID->GetUID(), "TEST_DEFINE1",);
	}
	*/
	App::Start();
	App::Stop();

	Sailor::App::Shutdown();

	return 0;
}
