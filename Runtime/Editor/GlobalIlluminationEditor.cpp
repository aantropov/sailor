#include "GlobalIlluminationEditor.h"
#include "GlobalIlluminationBakeController.h"
#include "Engine/World.h"
#include "Sailor.h"
#include "Submodules/Editor.h"

#include <utility>

using namespace Sailor;

namespace
{
	GlobalIlluminationECS* ResolveEditorGlobalIllumination(
		std::string* outDiagnostic = nullptr)
	{
		auto* editor = App::GetSubmodule<Editor>();
		auto* world = editor ? editor->GetWorld() : nullptr;
		auto* globalIllumination = world
			? world->GetECS<GlobalIlluminationECS>()
			: nullptr;
		if (!globalIllumination && outDiagnostic)
		{
			*outDiagnostic = "Global Illumination ECS is unavailable";
		}
		return globalIllumination;
	}
}

bool EditorRuntime::StartEditorGIProbesBake(
	const EditorGIProbesBakeRequest& request,
	std::string& outDiagnostic)
{
	return App::ExecuteOnEngineMainThread<bool>(
		false,
		[request, &outDiagnostic]()
		{
			auto* editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				outDiagnostic = "the Editor submodule is unavailable";
				return false;
			}
			return editor->StartGIProbesBake(request, outDiagnostic);
		});
}

bool EditorRuntime::CancelEditorGIProbesBake(std::string& outDiagnostic)
{
	auto* editor = App::GetSubmodule<Editor>();
	if (!editor)
	{
		outDiagnostic = "the Editor submodule is unavailable";
		return false;
	}
	return editor->CancelGIProbesBake(outDiagnostic);
}

bool EditorRuntime::GetEditorGIProbesBakeStatus(
	EditorGIProbesBakeStatus& outStatus)
{
	const auto* editor = App::GetSubmodule<Editor>();
	if (!editor)
	{
		outStatus = {};
		return false;
	}
	outStatus = editor->GetGIProbesBakeStatus();
	return true;
}

bool EditorRuntime::SetEditorGISettings(
	GISettings settings,
	std::string& outDiagnostic)
{
	return App::ExecuteOnEngineMainThread<bool>(
		false,
		[settings = std::move(settings), &outDiagnostic]() mutable
		{
			auto* editor = App::GetSubmodule<Editor>();
			auto* world = editor ? editor->GetWorld() : nullptr;
			if (!world)
			{
				outDiagnostic = "the current Editor world is unavailable";
				return false;
			}
			return world->SetGISettings(
				std::move(settings),
				outDiagnostic);
		});
}

bool EditorRuntime::GetEditorGlobalIlluminationState(
	EditorGlobalIlluminationState& outState)
{
	outState = {};
	return App::ExecuteOnEngineMainThread<bool>(
		false,
		[&outState]()
		{
			auto* globalIllumination = ResolveEditorGlobalIllumination();
			if (!globalIllumination)
			{
				return false;
			}
			outState.m_maxProbeStatesPerSnapshot =
				globalIllumination->GetMaxProbeStatesPerSnapshot();
			outState.m_mode = globalIllumination->GetWorldSettings().m_mode;
			outState.m_runtimeSettings =
				globalIllumination->GetWorldSettings().m_runtimeProbes;
			outState.m_runtimeStatus =
				globalIllumination->GetRuntimeGIProbesStatus();
			outState.m_bRuntimePreviewEnabled =
				globalIllumination->IsRuntimeGIProbesPreviewEnabled();
			outState.m_runtimeEditorBudget =
				globalIllumination->GetRuntimeGIProbesEditorBudget();
			outState.m_bEnabled = globalIllumination->IsEnabled();
			outState.m_probes = globalIllumination->GetProbeStates();
			outState.m_diagnostic = globalIllumination->GetDiagnostic();
			outState.m_compositionCount =
				globalIllumination->GetCompositionCount();
			outState.m_rejectedCompositionCount =
				globalIllumination->GetRejectedCompositionCount();
			return true;
		});
}

bool EditorRuntime::SetEditorRuntimeGIProbesPreviewEnabled(
	bool bEnabled,
	std::string& outDiagnostic)
{
	return App::ExecuteOnEngineMainThread<bool>(
		false,
		[bEnabled, &outDiagnostic]()
		{
			auto* globalIllumination = ResolveEditorGlobalIllumination(
				&outDiagnostic);
			if (!globalIllumination)
			{
				return false;
			}
			return globalIllumination->SetRuntimeGIProbesPreviewEnabled(
				bEnabled,
				outDiagnostic);
		});
}

bool EditorRuntime::SetEditorRuntimeGIProbesPaused(
	bool bPaused,
	std::string& outDiagnostic)
{
	return App::ExecuteOnEngineMainThread<bool>(
		false,
		[bPaused, &outDiagnostic]()
		{
			auto* globalIllumination = ResolveEditorGlobalIllumination(
				&outDiagnostic);
			if (!globalIllumination)
			{
				return false;
			}
			return globalIllumination->SetRuntimeGIProbesPaused(
				bPaused,
				outDiagnostic);
		});
}

bool EditorRuntime::SetEditorRuntimeGIProbesBudget(
	Settings::ERuntimeGIProbesEditorBudget budget,
	std::string& outDiagnostic)
{
	return App::ExecuteOnEngineMainThread<bool>(
		false,
		[budget, &outDiagnostic]()
		{
			auto* globalIllumination = ResolveEditorGlobalIllumination(
				&outDiagnostic);
			if (!globalIllumination)
			{
				return false;
			}
			return globalIllumination->SetRuntimeGIProbesEditorBudget(
				budget,
				outDiagnostic);
		});
}

bool EditorRuntime::RestartEditorRuntimeGIProbes(std::string& outDiagnostic)
{
	return App::ExecuteOnEngineMainThread<bool>(
		false,
		[&outDiagnostic]()
		{
			auto* globalIllumination = ResolveEditorGlobalIllumination(
				&outDiagnostic);
			if (!globalIllumination)
			{
				return false;
			}
			return globalIllumination->RestartRuntimeGIProbes(outDiagnostic);
		});
}

bool EditorRuntime::RebuildEditorRuntimeGIProbesScene(std::string& outDiagnostic)
{
	return App::ExecuteOnEngineMainThread<bool>(
		false,
		[&outDiagnostic]()
		{
			auto* globalIllumination = ResolveEditorGlobalIllumination(
				&outDiagnostic);
			if (!globalIllumination)
			{
				return false;
			}
			return globalIllumination->RebuildRuntimeGIProbesScene(
				outDiagnostic);
		});
}
