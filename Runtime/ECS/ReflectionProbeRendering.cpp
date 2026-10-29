#include "ECS/ReflectionProbeRendering.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "RHI/CommandList.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"

#include <algorithm>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

void ReflectionProbeRenderState::EnsureWarmup()
{
	if (m_warmup)
	{
		return;
	}
	m_warmup = Tasks::CreateTaskWithResult<bool>("Prepare reflection probe shaders"_h,
		[]()
		{
			if (!LocalReflectionCaptureJob::WarmupShaders())
			{
				return false;
			}
			auto registry = App::GetSubmodule<AssetRegistry>();
			auto compiler = App::GetSubmodule<ShaderCompiler>();
			for (const auto path : { "Shaders/ComputeEnvMap_IBL.shader", "Shaders/ComputeSheenEnvMap.shader" })
			{
				auto info = registry->GetAssetInfoPtr(path);
				ShaderSetPtr shader;
				if (!info || !compiler->LoadShader_Immediate(info->GetFileId(), shader) || !shader || !shader->IsReady())
				{
					return false;
				}
			}
			return true;
		}, EThreadType::Worker);
	m_warmup->Run();
}

void ReflectionProbeRenderState::CancelSlot(uint32_t slot)
{
	for (auto& work : m_work)
	{
		if (work.m_slot == slot && work.m_capture)
		{
			work.m_capture->m_bCancelled.store(true, std::memory_order_release);
		}
	}
}

void ReflectionProbeRenderState::Upsert(uint32_t slot, uint64_t slotGeneration,
	const LocalReflectionCaptureRequest& request, RHITexturePtr staticSource)
{
	if (slot >= MaxReflectionProbes || slotGeneration == 0u || !request.IsValid())
	{
		return;
	}
	m_lock.Lock();
	if (slotGeneration < m_slotGenerations[slot])
	{
		m_lock.Unlock();
		return;
	}
	m_slotGenerations[slot] = slotGeneration;
	auto& probe = m_probes[slot];
	const bool newSlot = probe.m_slotGeneration != slotGeneration;
	const bool sourceChanged = newSlot || probe.m_staticSource != staticSource ||
		probe.m_request.m_faceSize != request.m_faceSize;
	if (sourceChanged)
	{
		CancelSlot(slot);
	}
	if (newSlot)
	{
		probe = {};
	}
	probe.m_slotGeneration = slotGeneration;
	if (sourceChanged)
	{
		probe.m_configuration = ++m_configurations[slot];
		probe.m_pendingGeneration = staticSource ? ++m_generation : 0u;
		probe.m_requestedGeneration = probe.m_pendingGeneration;
	}
	probe.m_request = request;
	probe.m_staticSource = std::move(staticSource);
	if (probe.m_staticSource && probe.m_specular)
	{
		probe.m_capturePosition = glm::vec3(request.m_parameters.m_positionBlend);
	}
	PublishSnapshot();
	m_lock.Unlock();
}

void ReflectionProbeRenderState::Request(uint32_t slot, uint64_t slotGeneration)
{
	if (slot >= MaxReflectionProbes)
	{
		return;
	}
	m_lock.Lock();
	auto& probe = m_probes[slot];
	if (slotGeneration != 0u && probe.m_slotGeneration == slotGeneration && !probe.m_staticSource)
	{
		// Retain the newest request when Main advances without a renderable frame.
		// Captures already attached to a flight remain independent and can finish.
		probe.m_pendingGeneration = ++m_generation;
		probe.m_requestedGeneration = probe.m_pendingGeneration;
	}
	m_lock.Unlock();
}

void ReflectionProbeRenderState::Remove(uint32_t slot, uint64_t slotGeneration)
{
	if (slot >= MaxReflectionProbes)
	{
		return;
	}
	m_lock.Lock();
	if (m_probes[slot].m_slotGeneration == slotGeneration)
	{
		CancelSlot(slot);
		m_probes[slot] = {};
		PublishSnapshot();
	}
	m_lock.Unlock();
}

void ReflectionProbeRenderState::Reset()
{
	m_lock.Lock();
	for (uint32_t slot = 0u; slot < MaxReflectionProbes; ++slot)
	{
		CancelSlot(slot);
	}
	m_probes = {};
	PublishSnapshot();
	m_lock.Unlock();
}

RHISceneReflectionsSnapshotPtr ReflectionProbeRenderState::GetSnapshot() const
{
	m_lock.Lock();
	auto snapshot = m_snapshot;
	m_lock.Unlock();
	return snapshot;
}

ReflectionProbeRenderStatus ReflectionProbeRenderState::GetStatus(uint32_t slot) const
{
	if (slot >= MaxReflectionProbes)
	{
		return {};
	}
	m_lock.Lock();
	const auto& probe = m_probes[slot];
	const ReflectionProbeRenderStatus status{ probe.m_slotGeneration,
		probe.m_requestedGeneration, probe.m_publishedGeneration, probe.m_capturesSubmitted };
	m_lock.Unlock();
	return status;
}

void ReflectionProbeRenderState::PublishSnapshot()
{
	auto snapshot = TRefPtr<RHISceneReflectionsSnapshot>::Make();
	snapshot->m_revision = ++m_revision;
	for (uint32_t slot = 0u; slot < MaxReflectionProbes; ++slot)
	{
		const auto& probe = m_probes[slot];
		if (probe.m_slotGeneration == 0u || !probe.m_specular || !probe.m_sheen)
		{
			continue;
		}
		auto& data = snapshot->m_probes[slot];
		const auto& parameters = probe.m_request.m_parameters;
		data.m_positionRadius = glm::vec4(glm::vec3(parameters.m_positionBlend),
			(parameters.m_max.x - parameters.m_minEnabled.x) * 0.5f);
		data.m_capturePosition = glm::vec4(probe.m_capturePosition, 0.0f);
		data.m_textureIndices = glm::uvec4(slot * 2u, slot * 2u + 1u, 1u, 0u);
		snapshot->m_textures[slot * 2u] = probe.m_specular;
		snapshot->m_textures[slot * 2u + 1u] = probe.m_sheen;
	}
	m_snapshot = std::move(snapshot);
}

TVector<TRefPtr<LocalReflectionCaptureJob>> ReflectionProbeRenderState::Capture(const RHISceneViewPtr& source)
{
	TVector<TRefPtr<LocalReflectionCaptureJob>> captures;
	m_lock.Lock();
	const bool pending = std::any_of(m_probes.begin(), m_probes.end(),
		[](const auto& probe) { return probe.m_pendingGeneration != 0u; });
	if (!pending)
	{
		m_lock.Unlock();
		return captures;
	}
	EnsureWarmup();
	if (!m_warmup || !m_warmup->IsFinished() || !m_warmup->GetResult())
	{
		m_lock.Unlock();
		return captures;
	}
	for (uint32_t slot = 0u; slot < MaxReflectionProbes; ++slot)
	{
		auto& probe = m_probes[slot];
		if (probe.m_pendingGeneration == 0u)
		{
			continue;
		}
		Work work;
		work.m_slot = slot;
		work.m_slotGeneration = probe.m_slotGeneration;
		work.m_configuration = probe.m_configuration;
		work.m_generation = probe.m_pendingGeneration;
		work.m_request = probe.m_request;
		work.m_staticSource = probe.m_staticSource;
		if (!work.m_staticSource)
		{
			auto capture = TRefPtr<LocalReflectionCaptureJob>::Make();
			capture->m_request = work.m_request;
			capture->m_generation = work.m_generation;
			capture->AppendCameras(source);
			work.m_capture = capture;
			captures.Add(std::move(capture));
		}
		m_work.Add(std::move(work));
		probe.m_pendingGeneration = 0u;
	}
	m_lock.Unlock();
	return captures;
}

bool ReflectionProbeRenderState::IsCurrent(const Work& work) const
{
	const auto& probe = m_probes[work.m_slot];
	return probe.m_slotGeneration == work.m_slotGeneration &&
		probe.m_configuration == work.m_configuration;
}

void ReflectionProbeRenderState::Cancel(const TRefPtr<LocalReflectionCaptureJob>& capture)
{
	m_lock.Lock();
	for (size_t index = 0u; index < m_work.Num(); ++index)
	{
		auto& work = m_work[index];
		if (work.m_capture != capture)
		{
			continue;
		}
		if (IsCurrent(work))
		{
			auto& pending = m_probes[work.m_slot].m_pendingGeneration;
			pending = (std::max)(pending, work.m_generation);
		}
		auto cancelled = capture;
		cancelled->m_bCancelled.store(true, std::memory_order_release);
		m_work.RemoveAt(index);
		break;
	}
	m_lock.Unlock();
}

bool ReflectionProbeRenderState::StartFiltering(Work& work)
{
	auto& driver = Renderer::GetDriver();
	auto commands = Renderer::GetDriverCommands();
	const uint32_t size = work.m_request.m_faceSize;
	uint32_t levels = 1u;
	for (uint32_t mip = size; mip > 1u; mip >>= 1u)
	{
		++levels;
	}
	const ETextureUsageFlags usage = ETextureUsageBit::TextureTransferSrc_Bit |
		ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Storage_Bit | ETextureUsageBit::Sampled_Bit;
	const auto create = [&]()
	{
		return driver->CreateCubemap(glm::ivec2(size), levels, EFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Linear, ETextureClamping::Clamp, usage);
	};
	auto raw = work.m_raw ? work.m_raw : create();
	auto specular = create();
	auto sheen = create();
	auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
	if (!raw || !specular || !sheen || !command)
	{
		return false;
	}
	driver->SetDebugName(command, "Reflection probe filtering"_h);
	commands->BeginCommandList(command, true);
	if (work.m_staticSource)
	{
		commands->ImageMemoryBarrier(command, raw, EImageLayout::ComputeWrite);
		if (!commands->ConvertEquirect2Cubemap(command, work.m_staticSource, raw))
		{
			commands->EndCommandList(command);
			return false;
		}
		commands->ImageMemoryBarrier(command, raw, EImageLayout::TransferDstOptimal);
		const uint32_t before = command->GetNumRecordedCommands();
		commands->GenerateMipMaps(command, raw);
		if (before == command->GetNumRecordedCommands())
		{
			commands->EndCommandList(command);
			return false;
		}
	}
	const auto filter = [&](const RHICubemapPtr& target, bool isSheen)
	{
		const uint32_t first = isSheen ? 0u : 1u;
		if (!isSheen)
		{
			commands->ImageMemoryBarrier(command, raw, EImageLayout::TransferSrcOptimal);
			commands->ImageMemoryBarrier(command, target, EImageLayout::TransferDstOptimal);
			if (!commands->BlitImage(command, raw, target, glm::ivec4(0, 0, size, size), glm::ivec4(0, 0, size, size)))
			{
				return false;
			}
		}
		commands->ImageMemoryBarrier(command, raw, EImageLayout::ShaderReadOnlyOptimal);
		commands->ImageMemoryBarrier(command, target, EImageLayout::ComputeWrite);
		for (uint32_t level = first; level < levels; ++level)
		{
			auto bindings = driver->CreateShaderBindings();
			const RHITexturePtr mip = level == 0u ? target : target->GetMipLevel(level);
			TVector<RHITexturePtr> outputs;
			// Each dispatch addresses output zero. Repeating that view also keeps
			// the existing fixed-size shader interface valid at every resolution.
			for (uint32_t output = 0u; output < (isSheen ? 8u : 9u); ++output)
			{
				outputs.Add(mip);
			}
			driver->AddSamplerToShaderBindings(bindings, "rawEnvMap"_h, raw, 0u);
			driver->AddStorageImageToShaderBindings(bindings, isSheen ? "sheenEnvMap"_h : "envMap"_h, outputs, 1u);
			bindings->RecalculateCompatibility();
			struct PushConstants
			{
				int32_t m_level;
				float m_roughness;
			};
			const PushConstants push{ 0, float(level) / float(levels - 1u) };
			const uint32_t groupSize = isSheen ? 16u : 32u;
			const uint32_t groups = (std::max)(1u, ((size >> level) + groupSize - 1u) / groupSize);
			const uint32_t before = command->GetNumRecordedCommands();
			commands->Dispatch(command, (isSheen ? m_sheenShader : m_specularShader)->GetComputeShaderRHI(),
				groups, groups, 6u, { bindings }, &push, sizeof(push));
			if (command->GetNumRecordedCommands() == before)
			{
				return false;
			}
		}
		commands->ImageMemoryBarrier(command, target, EImageLayout::ShaderReadOnlyOptimal);
		return true;
	};
	const bool recorded = filter(specular, false) && filter(sheen, true);
	commands->ImageMemoryBarrier(command, raw, EImageLayout::ShaderReadOnlyOptimal);
	commands->EndCommandList(command);
	if (!recorded)
	{
		return false;
	}
	auto completion = RHIFencePtr::Make();
	if (!driver->SubmitCommandList(command, completion))
	{
		return false;
	}
	work.m_raw = std::move(raw);
	work.m_specular = std::move(specular);
	work.m_sheen = std::move(sheen);
	work.m_filterCompletion = std::move(completion);
	return true;
}

void ReflectionProbeRenderState::Poll(const RHIFrameGraphPtr& graph)
{
	if (!graph)
	{
		return;
	}
	m_lock.Lock();
	if (m_work.IsEmpty() || !m_warmup || !m_warmup->IsFinished() || !m_warmup->GetResult())
	{
		m_lock.Unlock();
		return;
	}
	if (!m_specularShader || !m_sheenShader)
	{
		auto registry = App::GetSubmodule<AssetRegistry>();
		auto compiler = App::GetSubmodule<ShaderCompiler>();
		for (const auto entry : { std::pair("Shaders/ComputeEnvMap_IBL.shader", &m_specularShader),
			std::pair("Shaders/ComputeSheenEnvMap.shader", &m_sheenShader) })
		{
			auto info = registry->GetAssetInfoPtr(entry.first);
			if (info)
			{
				compiler->LoadShader(info->GetFileId(), *entry.second);
			}
		}
	}
	if (!m_specularShader || !m_sheenShader || !m_specularShader->IsReady() || !m_sheenShader->IsReady())
	{
		m_lock.Unlock();
		return;
	}
	for (size_t index = 0u; index < m_work.Num();)
	{
		auto& work = m_work[index];
		if (!IsCurrent(work))
		{
			m_work.RemoveAt(index);
			continue;
		}
		auto& probe = m_probes[work.m_slot];
		if (work.m_filterCompletion)
		{
			const auto status = work.m_filterCompletion->GetStatus();
			if (status == EFenceStatus::Pending)
			{
				++index;
				continue;
			}
			if (status == EFenceStatus::Finished && work.m_generation > probe.m_publishedGeneration)
			{
				probe.m_specular = work.m_specular;
				probe.m_sheen = work.m_sheen;
				const auto& captureRequest = work.m_staticSource ? probe.m_request : work.m_request;
				probe.m_capturePosition = glm::vec3(captureRequest.m_parameters.m_positionBlend);
				probe.m_publishedGeneration = work.m_generation;
				PublishSnapshot();
			}
			else if (status == EFenceStatus::Failed)
			{
				probe.m_pendingGeneration = (std::max)(probe.m_pendingGeneration, work.m_generation);
			}
			m_work.RemoveAt(index);
			continue;
		}
		if (work.m_capture)
		{
			const auto& capture = work.m_capture;
			if (!capture->m_submission || capture->m_submission->IsPending() ||
				(capture->m_completion && capture->m_completion->GetStatus() == EFenceStatus::Pending))
			{
				++index;
				continue;
			}
			if (!capture->m_completion || !capture->m_bSubmitted || !capture->m_submission->IsSuccessful() ||
				capture->m_completion->GetStatus() != EFenceStatus::Finished)
			{
				if (!capture->m_bCancelled.load(std::memory_order_acquire))
				{
					probe.m_pendingGeneration = (std::max)(probe.m_pendingGeneration, work.m_generation);
				}
				m_work.RemoveAt(index);
				continue;
			}
			work.m_raw = capture->m_cubemap;
			++probe.m_capturesSubmitted;
			work.m_capture.Clear();
		}
		if (work.m_generation <= probe.m_publishedGeneration)
		{
			m_work.RemoveAt(index);
			continue;
		}
		if (!work.m_staticSource || work.m_staticSource->IsReady())
		{
			if (!StartFiltering(work))
			{
				// Retry the newest request on a future frame, rather than retaining
				// one failed allocation/dispatch for every EveryFrame submission.
				probe.m_pendingGeneration = (std::max)(probe.m_pendingGeneration, work.m_generation);
				m_work.RemoveAt(index);
				continue;
			}
		}
		++index;
	}
	m_lock.Unlock();
}
