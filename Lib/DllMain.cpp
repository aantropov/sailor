struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

// dllmain.cpp : Defines the entry point for the DLL application.
#include <windows.h>
#include "Sailor.h"
#include "Submodules/Editor.h"
#include "Core/Reflection.h"

extern "C"
{
	SAILOR_API void Initialize(const char** commandLineArgs, int32_t num)
	{
		Sailor::App::Initialize(commandLineArgs, num);
	}

	SAILOR_API void Start()
	{
		Sailor::App::Start();
	}

	SAILOR_API void Stop()
	{
		Sailor::App::Stop();
	}

	SAILOR_API void Shutdown()
	{
		Sailor::App::Shutdown();
	}

	SAILOR_API void SetWindowPosition(uint32_t windowPosX, uint32_t windowPosY, uint32_t width, uint32_t height)
	{
		auto editor = Sailor::App::GetSubmodule<Sailor::Editor>();

		RECT rect{};
		rect.left = windowPosX;
		rect.right = windowPosX + width;

		rect.bottom = windowPosY;
		rect.top = windowPosY + height;

		editor->SetWindowRect(rect);
	}

	SAILOR_API uint32_t GetMessages(char** messages, uint32_t num)
	{
		auto editor = Sailor::App::GetSubmodule<Sailor::Editor>();
		uint32_t numMsg = std::min((uint32_t)editor->NumMessages(), num);

		for (uint32_t i = 0; i < numMsg; i++)
		{
			std::string msg;
			if (editor->PullMessage(msg))
			{
				messages[i] = new char[msg.size() + 1];
				if (messages[i] == nullptr)
				{
					// Handle allocation failure
					return i;
				}

				std::copy(msg.begin(), msg.end(), messages[i]);
				messages[i][msg.size()] = '\0';
			}
			else
			{
				return i;
			}
		}

		return numMsg;
	}

	SAILOR_API uint32_t SerializeCurrentWorld(char** yamlNode)
	{
		auto editor = Sailor::App::GetSubmodule<Sailor::Editor>();
		auto node = editor->SerializeWorld();

		if (!node.IsNull())
		{
			std::string serializedNode = YAML::Dump(node);
			size_t length = serializedNode.length();

			yamlNode[0] = new char[length + 1];
			strcpy_s(yamlNode[0], length + 1, serializedNode.c_str());
			yamlNode[0][length] = '\0';

			return static_cast<uint32_t>(length);
		}
		else
		{
			yamlNode[0] = nullptr;
			return 0;
		}
	}

	SAILOR_API uint32_t SerializeEngineTypes(char** yamlNode)
	{
		auto node = Sailor::Reflection::ExportEngineTypes();

		if (!node.IsNull())
		{
			std::string serializedNode = YAML::Dump(node);
			size_t length = serializedNode.length();

			yamlNode[0] = new char[length + 1];
			strcpy_s(yamlNode[0], length + 1, serializedNode.c_str());
			yamlNode[0][length] = '\0';

			return static_cast<uint32_t>(length);
		}
		else
		{
			yamlNode[0] = nullptr;
			return 0;
		}
	}
}

BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

