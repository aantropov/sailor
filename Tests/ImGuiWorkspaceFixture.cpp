#include "Support/ImGuiWorkspaceProbe.h"
#include "Components/Component.h"
#include "Engine/EngineLoop.h"
#include "Engine/GameObject.h"
#include "Submodules/ImGuiApi.h"
#include "Tasks/Scheduler.h"
#include "Workspace/WorkspaceTypeRegistration.h"
#include <cstring>

namespace
{
	Sailor::Tests::ImGuiWorkspaceProbe* g_probe = nullptr;

	struct ModuleLifetime
	{
		~ModuleLifetime()
		{
			if (!g_probe) return;
			g_probe->m_bWasModuleUnloaded = true;
			g_probe->m_bWasUnloadedAfterContext = !Sailor::ImGuiApi::GetCurrentContext() &&
				g_probe->m_callbacksStarted == g_probe->m_callbacksFinished;
		}
	} g_lifetime;

	struct CallbackData
	{
		Sailor::Tests::ImGuiWorkspaceProbe* m_probe;
		uint32_t m_frame;
	};

	void ReadFrame(const ImDrawList*, const ImDrawCmd* command)
	{
		CallbackData data{};
		std::memcpy(&data, command->UserCallbackData, sizeof(data));
		auto& probe = *data.m_probe;
		++probe.m_callbacksStarted;
		probe.m_callbacksStarted.notify_one();
		probe.m_bIsCallbackReleased.wait(false);
		std::memcpy(&data, command->UserCallbackData, sizeof(data));
		if (data.m_probe != &probe || data.m_frame == 0 || data.m_frame > probe.m_frames ||
			!Sailor::ImGuiApi::GetCurrentContext() ||
			Sailor::App::GetSubmodule<Sailor::Tasks::Scheduler>()->GetCurrentThreadType() != Sailor::EThreadType::RHI)
			probe.m_bIsCallbackDataValid = false;
		if (command->UserCallbackDataSize == 0) ++probe.m_borrowedCallbacks;
		else ++probe.m_copiedCallbacks;
		++probe.m_callbacksFinished;
	}
}

namespace ImGuiWorkspace
{
	class FixtureComponent final : public Sailor::Component
	{
		SAILOR_WORKSPACE_REFLECTABLE(FixtureComponent)

	public:
		void EditorTick(float) override
		{
			auto* context = Sailor::ImGuiApi::GetCurrentContext();
			if (ImGui::GetCurrentContext() != context)
			{
				ImGuiMemAllocFunc allocate{};
				ImGuiMemFreeFunc free{};
				void* userData{};
				Sailor::ImGuiApi::GetAllocatorFunctions(&allocate, &free, &userData);
				ImGui::SetAllocatorFunctions(allocate, free, userData);
				ImGui::SetCurrentContext(context);
			}
			CallbackData data{ g_probe, ++g_probe->m_frames };
			if (!m_bHasDrawn)
			{
				m_borrowedCallback = data;
				m_bHasDrawn = true;
			}
			auto* draw = ImGui::GetForegroundDrawList();
			draw->AddText({ 8, 8 }, IM_COL32_WHITE, "Workspace ImGui lifecycle");
			draw->AddCallback(ReadFrame, &data, sizeof(data));
			draw->AddCallback(ReadFrame, &m_borrowedCallback);
		}

		~FixtureComponent() override
		{
			if (m_bHasDrawn)
			{
				g_probe->m_bWasComponentDestroyedWithContext = ImGui::GetCurrentContext() &&
					ImGui::GetCurrentContext() == Sailor::ImGuiApi::GetCurrentContext();
				g_probe->m_bWasComponentDestroyedAfterCallbacks =
					g_probe->m_callbacksStarted == g_probe->m_callbacksFinished;
			}
		}

	private:
		CallbackData m_borrowedCallback{};
		bool m_bHasDrawn = false;
	};

	using WorkspaceTypes = Sailor::Workspace::TWorkspaceTypeList<FixtureComponent>;
}

REFL_AUTO(type(ImGuiWorkspace::FixtureComponent, bases<Sailor::Component>))

namespace
{
	constexpr char WorkspaceModuleName[] = "ImGuiWorkspaceFixture";

	uint32_t SAILOR_WORKSPACE_CALL RegisterWorkspaceTypes(const Sailor::Workspace::WorkspaceHostApiV1* hostApi) noexcept
	{
		return Sailor::Workspace::RegisterWorkspaceTypesV1<ImGuiWorkspace::WorkspaceTypes>(hostApi);
	}
}

extern "C" SAILOR_WORKSPACE_MODULE_EXPORT void ConfigureImGuiWorkspaceProbe(Sailor::Tests::ImGuiWorkspaceProbe* probe)
{
	g_probe = probe;
	probe->m_bHasPrivateContext = ImGui::GetCurrentContext() != Sailor::ImGuiApi::GetCurrentContext();
	Sailor::App::GetSubmodule<Sailor::EngineLoop>()->GetWorld()->Instantiate("Workspace ImGui fixture")
		->AddComponent<ImGuiWorkspace::FixtureComponent>();
}

extern "C" SAILOR_WORKSPACE_MODULE_EXPORT const Sailor::Workspace::WorkspaceModuleApiV1* SAILOR_WORKSPACE_CALL
	SailorGetWorkspaceModuleApiV1() noexcept
{
	static const Sailor::Workspace::WorkspaceModuleApiV1 api
	{
		static_cast<uint32_t>(sizeof(Sailor::Workspace::WorkspaceModuleApiV1)),
		Sailor::Workspace::WorkspaceModuleApiVersion,
		WorkspaceModuleName,
		static_cast<uint64_t>(sizeof(WorkspaceModuleName) - 1),
		Sailor::Workspace::GetWorkspaceModuleAbiTagV1(),
		Sailor::Workspace::GetWorkspaceModuleAbiTagV1Length(),
		&RegisterWorkspaceTypes
	};
	return &api;
}
