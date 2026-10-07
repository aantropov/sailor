#include "RenderStats.h"
#include "Engine/World.h"
#include "ECS/LightingECS.h"
#include "RHI/Renderer.h"
#include "Settings/GraphicsSettings.h"

#include <imgui.h>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

using namespace Sailor;

namespace
{
	void DrawViewportStatsOverlay(
		uint32_t cpuFps,
		uint32_t renderFps,
		uint32_t presentFps,
		uint32_t numBatches,
		uint32_t numInstances,
		float shadowMemoryMb,
		float csmShadowMemoryMb,
		float localShadowMemoryMb,
		float shadowMemoryBudgetMb,
		const RHI::RHIGlobalIlluminationRenderStats& globalIlluminationStats,
		const RHI::Stats& stats,
		const char* gpuQueryText)
	{
		const ImGuiIO& io = ImGui::GetIO();
		if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
		{
			return;
		}

		constexpr float BytesToMb = 1.0f / (1024.0f * 1024.0f);
		constexpr float BytesToKb = 1.0f / 1024.0f;
		const uint32_t globalIlluminationFlightSlot =
			globalIlluminationStats.m_flightSlot;
		char globalIlluminationFlight[16];
		if (globalIlluminationFlightSlot ==
			(std::numeric_limits<uint32_t>::max)())
		{
			std::snprintf(
				globalIlluminationFlight,
				sizeof(globalIlluminationFlight),
				"-");
		}
		else
		{
			std::snprintf(
				globalIlluminationFlight,
				sizeof(globalIlluminationFlight),
				"%u",
				globalIlluminationFlightSlot);
		}

		char text[2048];
		const char* globalIlluminationStatus =
			!globalIlluminationStats.m_bEnabled ||
			globalIlluminationStats.m_mode == EGlobalIlluminationMode::NoGI
				? "disabled"
				: globalIlluminationStats.m_bActive
					? globalIlluminationStats.m_mode ==
						EGlobalIlluminationMode::Runtime
						? "runtime"
						: "baked"
					: "fallback";
		const std::string_view globalIlluminationModeName = magic_enum::enum_name(globalIlluminationStats.m_mode);
		std::snprintf(
			text,
			sizeof(text),
			"CPU %u FPS\nRender %u FPS\nPresent %u /s\nBatches %u\nInstances %u\n"
			"Shadows %.1f / %.0f MB\n  CSM %.1f MB\n  Local %.1f MB\n"
			"GI %s (%.*s) rev %llu flight %s\n"
			"  States %u / %u, bricks %u / %u, probes %u\n"
			"  CPU payload %.2f MB, GPU/flight %.2f MB\n"
			"  Copy %.1f KB, upload %.1f KB\n"
			"GPU memory\n  Materials %.1f MB\n  Textures %.1f MB\n  Meshes %.1f MB\n  General %.1f MB%s%s",
			cpuFps,
			renderFps,
			presentFps,
			numBatches,
			numInstances,
			shadowMemoryMb,
			shadowMemoryBudgetMb,
			csmShadowMemoryMb,
			localShadowMemoryMb,
			globalIlluminationStatus,
			static_cast<int>(globalIlluminationModeName.size()), globalIlluminationModeName.empty() ? "" : globalIlluminationModeName.data(),
			static_cast<unsigned long long>(
				globalIlluminationStats.m_activeRevision),
			globalIlluminationFlight,
			globalIlluminationStats.m_stateCount,
			globalIlluminationStats.m_qualityBudget,
			globalIlluminationStats.m_loadedBricks,
			globalIlluminationStats.m_totalBricks,
			globalIlluminationStats.m_probeCount,
			globalIlluminationStats.m_cpuPayloadBytes * BytesToMb,
			globalIlluminationStats.m_gpuAllocatedBytes * BytesToMb,
			globalIlluminationStats.m_copiedCpuBytes * BytesToKb,
			globalIlluminationStats.m_uploadedGpuBytes * BytesToKb,
			stats.m_materialsMemoryUsage.load(std::memory_order_relaxed) * BytesToMb,
			stats.m_texturesMemoryUsage.load(std::memory_order_relaxed) * BytesToMb,
			stats.m_meshesMemoryUsage.load(std::memory_order_relaxed) * BytesToMb,
			stats.m_generalMemoryUsage.load(std::memory_order_relaxed) * BytesToMb,
			gpuQueryText && gpuQueryText[0] != '\0' ? "\n" : "",
			gpuQueryText ? gpuQueryText : "");

		constexpr float Margin = 10.0f;
		const ImVec2 textSize = ImGui::CalcTextSize(text);
		const ImVec2 position(
			std::max(Margin, io.DisplaySize.x - textSize.x - Margin),
			Margin);

		ImGui::GetForegroundDrawList()->AddText(position, IM_COL32(160, 160, 160, 255), text);
	}
}

void Sailor::DrawRenderStats(uint32_t cpuFps, const TVector<TSharedPtr<World>>& worlds,
	const RHI::Renderer& renderer, Settings::ERenderStatsMode statsMode)
{
	if (statsMode == Settings::ERenderStatsMode::None) return;

	float shadowMemoryMb = 0.0f;
	float csmShadowMemoryMb = 0.0f;
	float localShadowMemoryMb = 0.0f;
	float shadowMemoryBudgetMb = 0.0f;
	for (const auto& world : worlds)
	{
		if (auto lighting = world->GetECS<LightingECS>())
		{
			shadowMemoryMb += lighting->GetShadowsOccupiedMemoryMb();
			csmShadowMemoryMb += lighting->GetCsmShadowsOccupiedMemoryMb();
			localShadowMemoryMb += lighting->GetLocalShadowsOccupiedMemoryMb();
			shadowMemoryBudgetMb += lighting->GetShadowsMemoryBudgetMb();
		}
	}

	const auto& stats = renderer.GetStats();
	const RHI::RHIGlobalIlluminationRenderStats globalIlluminationStats =
		renderer.GetGlobalIlluminationRenderStats();
	std::string gpuQueryText;
	if (statsMode == Settings::ERenderStatsMode::RenderStatsAndQueries)
	{
		if (!renderer.GetDriver()->SupportsGpuFrameTimeQueries())
		{
			gpuQueryText = "GPU queries unavailable";
		}
		else
		{
			const auto gpuTimings = renderer.GetGpuTimings();
			if (gpuTimings.m_bValid)
			{
				char frameTimeText[96]{};
				std::snprintf(
					frameTimeText,
					sizeof(frameTimeText),
					"GPU work (sum) %.2f ms, sample %.0f ms old",
					gpuTimings.m_gpuWorkMilliseconds,
					gpuTimings.GetAgeMilliseconds(std::chrono::steady_clock::now()));
				gpuQueryText = frameTimeText;

				const auto& topGpuTimings = gpuTimings.m_timings;
				if (topGpuTimings.IsEmpty())
				{
					gpuQueryText += "\nGPU nodes/ops not recorded";
				}
				else
				{
					gpuQueryText += "\nSlowest GPU nodes (avg):";
					for (size_t i = 0u; i < std::min<size_t>(3u, topGpuTimings.Num()); ++i)
					{
						char timingText[160]{};
						const std::string_view queue = magic_enum::enum_name(topGpuTimings[i].m_queue);
						std::snprintf(
							timingText,
							sizeof(timingText),
							"\n%zu. %.64s [%.*s] %.3f ms",
							i + 1u,
							topGpuTimings[i].m_name.ToString().c_str(),
							static_cast<int>(queue.size()), queue.empty() ? "" : queue.data(),
							topGpuTimings[i].m_durationMilliseconds);
						gpuQueryText += timingText;
					}
				}
			}
			else
			{
				gpuQueryText = gpuTimings.m_queryId == 0u ? "GPU query pending" : "GPU query invalid";
			}
		}
	}
	DrawViewportStatsOverlay(
		cpuFps,
		stats.m_renderFps.load(std::memory_order_relaxed),
		stats.m_presentFps.load(std::memory_order_relaxed),
		stats.m_numBatches.load(std::memory_order_relaxed),
		stats.m_numInstances.load(std::memory_order_relaxed),
		shadowMemoryMb,
		csmShadowMemoryMb,
		localShadowMemoryMb,
		shadowMemoryBudgetMb,
		globalIlluminationStats,
		stats,
		gpuQueryText.c_str());
}
