struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#include <wtypes.h>
#include "Sailor.h"
#include "AssetRegistry/ShaderCompiler.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int32_t)
{
	auto assetRegistry = AssetRegistry::GetInstance();
	auto shaderCompiler = ShaderCompiler::GetInstance();

	EngineInstance::Initialize();
	
	/*if (auto shaderUID = assetRegistry->GetAssetInfo("Shaders\\Simple.shader"))
	{
		ShaderCompiler::GetInstance()->CompileAllPermutations(shaderUID->GetUID());

		//ShaderCompiler::GetInstance()->GetSpirvCode(shaderUID->GetUID(), "TEST_DEFINE1",);
	}
	*/
	EngineInstance::Start();
	EngineInstance::Shutdown();

	return 0;
}
