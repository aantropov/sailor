#include <wtypes.h>
#include "Sailor.h"
#include "AssetRegistry/ShaderCompiler.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	EngineInstance::Initialize();

	auto assetRegistry = AssetRegistry::GetInstance();
	auto shaderCompiler= ShaderCompiler::GetInstance();
	
	if (auto shaderUID = assetRegistry->GetAssetInfo("Shaders\\Simple.shader"))
	{
		auto shader = ShaderCompiler::GetInstance()->LoadShader(shaderUID->GetUID());
		ShaderCompiler::GetInstance()->GeneratePrecompiledGlslPermutations(shaderUID->GetUID());
	}

	EngineInstance::Start();
	EngineInstance::Shutdown();

	return 0;
}
