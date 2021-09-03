struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#include <wtypes.h>
#include "Sailor.h"
#include "AssetRegistry/ShaderCompiler.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Core/SharedPtr.hpp"
#include "Core/WeakPtr.hpp"

using namespace Sailor;

class A
{
public:
	A() = default;
	A(int ina) : a(ina) {}

	int a = 3;
};

class B : public A
{
	int a = 5;
};

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int32_t)
{
	{
		TSharedPtr<A> a1;
		TSharedPtr<B> b2 = TSharedPtr<B>::Make();

		a1 = b2;

		TWeakPtr<A> w1 = a1;
		TWeakPtr<B> w2 = b2;
		w1 = w2;

		w2.Lock();
		w2.Clear();

		a1.Clear();
		b2.Clear();
	}

	EngineInstance::Initialize();

	auto assetRegistry = AssetRegistry::GetInstance();
	auto shaderCompiler= ShaderCompiler::GetInstance();
	
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
