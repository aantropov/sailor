#include "Sailor.h"
#include "Engine/World.h"
#include "TextureImporterTestAccess.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "RHI/Material.h"
#include "RHI/Renderer.h"

#include <array>
#include <chrono>
#include <cstring>
#include <latch>
#include <thread>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace Sailor;

namespace
{
	void Require(bool value, const char* message)
	{
		if (!value) throw std::runtime_error(message);
	}

	void Drain()
	{
		App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(
			{ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
		Require(GraphicsDriver::Vulkan::VulkanApi::GetInstance()->GetMainDevice()->WaitIdle() == VK_SUCCESS,
			"material test GPU work must finish");
		RHI::Renderer::GetDriver()->TrackResources_ThreadSafe();
	}

	FileId WriteTexture(const std::filesystem::path& workspace, const char* name, bool valid)
	{
		const auto path = workspace / "Content" / (std::string(name) + ".tga");
		std::array<uint8_t, 21> bytes{};
		if (valid)
		{
			bytes[2] = 2;
			bytes[12] = bytes[14] = 1;
			bytes[16] = 24;
			bytes[20] = 255;
		}
		std::ofstream output(path, std::ios::binary);
		output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		output.close();
		Require(static_cast<bool>(output), "material texture fixture must be written");
		return App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
	}

	struct MaterialFixture
	{
		MaterialFixture(const std::filesystem::path& workspace, const char* name, FileId texture,
			FileId shader = FileId("1A4BA353-FDA4-4F65-941F-D9FFEE4630A0")) :
			path(workspace / "Content" / (std::string(name) + ".mat"))
		{
			MaterialAsset::Data data;
			data.m_shader = shader;
			data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(1, 0.5f, 0.25f, 1);
			data.m_uniformsFloat["material.roughnessFactor"] = 0.75f;
			if (texture) data.m_samplers["baseColorSampler"] = texture;
			id = App::GetSubmodule<MaterialImporter>()->CreateMaterialAsset(path.string(), std::move(data));
			info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(id);
			Require(info != nullptr, "material fixture must register");
		}

		YAML::Node Read() const { return YAML::LoadFile(path.string()); }
		void Write(const YAML::Node& document) const
		{
			std::ofstream output(path);
			output << document;
			Require(static_cast<bool>(output), "material fixture update must be written");
		}
		MaterialPtr Load() const
		{
			MaterialPtr material;
			Require(App::GetSubmodule<MaterialImporter>()->LoadMaterial_Immediate(id, material) && material,
				"material fixture must load");
			Drain();
			Require(material->IsReady(), "fixture GPU material must become ready");
			return material;
		}
		void Reload() const
		{
			App::GetSubmodule<MaterialImporter>()->OnUpdateAssetInfo(info, true);
		}

		std::filesystem::path path;
		FileId id;
		AssetInfoPtr info;
	};

	void SetColor(YAML::Node& document, glm::vec4 color)
	{
		auto values = document["uniformsVec4"].as<TMap<std::string, glm::vec4>>();
		values["material.baseColorFactor"] = color;
		document["uniformsVec4"] = values;
	}

	glm::vec4 Color(MaterialPtr material)
	{
		glm::vec4 value;
		Require(material->GetUniformsVec4().TryGet("material.baseColorFactor", value), "material color must exist");
		return value;
	}

	TVector<uint8_t> ReadGpu(RHI::RHIShaderBindingSetPtr bindings)
	{
		TVector<uint8_t> bytes;
		auto read = Tasks::CreateTask("Read material fixture GPU bytes", [&]()
			{
				using namespace RHI;
				auto& driver = Renderer::GetDriver();
				auto* commands = Renderer::GetDriverCommands();
				auto binding = bindings->GetOrAddShaderBinding("material");
				Require(binding && binding->m_vulkan.m_valueBinding, "material must own reflected storage");
				const size_t size = (std::max)(binding->GetLayout().m_size, binding->GetLayout().m_paddedSize);
				Require(size > 0, "material storage must have a reflected size");
				auto readback = driver->CreateBuffer(size, EBufferUsageBit::BufferTransferDst_Bit,
					EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
				auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(cmd, true);
				cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
				cmd->m_vulkan.m_commandBuffer->CopyBuffer(*binding->m_vulkan.m_valueBinding->Get(),
					*readback->m_vulkan.m_buffer->Get(), size);
				cmd->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
				commands->EndCommandList(cmd);
				auto fence = RHIFencePtr::Make();
				Require(driver->SubmitCommandList(cmd, fence), "material readback must submit");
				Require(fence->Wait(5000000000ull) == EFenceStatus::Finished, "material readback must complete");
				bytes.Resize(size);
				std::memcpy(bytes.GetData(), readback->GetPointer(), size);
				fence->ClearDependencies();
			}, EThreadType::Render);
		read->Run();
		read->Wait();
		return bytes;
	}

	void CheckGpuColor(RHI::RHIShaderBindingSetPtr bindings, const TVector<uint8_t>& bytes, glm::vec4 expected)
	{
		RHI::ShaderLayoutBindingMember member;
		Require(bindings->GetOrAddShaderBinding("material")->FindVariableInUniformBuffer("baseColorFactor", member) &&
			member.m_absoluteOffset + sizeof(expected) <= bytes.Num(), "color must fit reflected GPU storage");
		glm::vec4 value;
		std::memcpy(&value, bytes.GetData() + member.m_absoluteOffset, sizeof(value));
		Require(value == expected, "GPU material color must match authored values");
	}

	FileId WriteShader(const std::filesystem::path& workspace, const char* name, bool valid = true, bool compute = false,
		const char* include = nullptr)
	{
		const auto path = workspace / "Content" / (std::string(name) + ".shader");
		YAML::Node shader;
		shader["glslCommon"] = "#version 450\n";
		if (include) shader["includes"].push_back(include);
		if (compute)
		{
			shader["glslCompute"] = valid ? "layout(local_size_x = 1) in; void main() {}" : "this is not valid GLSL";
		}
		else
		{
			shader["colorAttachments"].push_back("R8G8B8A8_UNORM");
			shader["defines"].push_back("MOTIONS");
			shader["defines"].push_back("EXTRA");
			shader["glslVertex"] = "layout(location = 0) in vec3 position; void main() { gl_Position = vec4(position, 1); }";
			shader["glslFragment"] = valid ? R"glsl(
struct MaterialData {
	vec4 baseColorFactor;
	float roughnessFactor;
	float alphaCutoff;
	uint baseColorSampler;
#ifdef EXTRA
	vec4 emission;
#endif
};
layout(std430, set = 3, binding = 0) readonly buffer MaterialBuffer { MaterialData data[1]; } material;
layout(location = 0) out vec4 outColor;
void main() {
	outColor = material.data[0].baseColorFactor * vec4(material.data[0].roughnessFactor,
		material.data[0].alphaCutoff, float(material.data[0].baseColorSampler), 1);
#ifdef EXTRA
	outColor += material.data[0].emission;
#endif
}
)glsl" : "this is not valid GLSL";
		}
		std::ofstream output(path);
		output << shader;
		output.close();
		Require(static_cast<bool>(output), "shader fixture must be written");
		return App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
	}

	void TestWarmShaderPermutation(const std::filesystem::path& workspace)
	{
		auto* compiler = App::GetSubmodule<ShaderCompiler>();
		auto& cache = ShaderCompilerTestAccess::GetShaderCache(*compiler);
		for (bool compute : { false, true })
		{
			const auto uid = WriteShader(workspace, compute ? "WarmCompute" : "WarmGraphics", true, compute);
			ShaderSetPtr shader;
			Require(compiler->LoadShader_Immediate(uid, shader) && shader && shader->IsReady(),
				"warm shader fixture must compile and create its regular/debug RHI stages");
			Drain();
			auto update = [&]()
			{
				bool success = false;
				auto task = Tasks::CreateTask("Rebuild cached shader RHI", [&]()
					{
						success = ShaderCompilerTestAccess::UpdateRHIResource(*compiler, shader, 0);
					}, EThreadType::Render);
				task->Run();
				task->Wait();
				Drain();
				return success;
			};
			const uint64_t expectedReads = compute ? 2 : 4;
			const auto generation = ShaderCacheTestAccess::GetGeneration(cache, uid, 0);
			ShaderCacheTestAccess::TakeArtifactReadCount(cache);
			Require(update() && shader->IsReady(), "RHI update must publish a complete shader set");
			const auto reads = ShaderCacheTestAccess::TakeArtifactReadCount(cache);
			Require(reads == expectedReads, "one real RHI update must read each regular/debug artifact exactly once");
			Require(ShaderCacheTestAccess::GetGeneration(cache, uid, 0) == generation,
				"a warm RHI update must not recompile the shader");
			const auto damaged = ShaderCacheTestAccess::GetArtifactPath(cache, uid, 0,
				compute ? ShaderCache::ComputeShaderTag : ShaderCache::FragmentShaderTag, true);
			std::filesystem::resize_file(damaged, 7);
			Require(update() && shader->IsReady(), "a broken artifact must recompile to a complete RHI set");
			const auto repairedGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 0);
			Require(repairedGeneration != generation, "a broken debug artifact must rebuild the complete permutation");
			ShaderCacheTestAccess::FailNextArtifactCleanup(cache);
			Require(ShaderCompilerTestAccess::SaveCacheAndCombineResult(cache, true) &&
				!cache.IsDirty() && cache.NeedsMaintenance() && shader->IsReady(),
				"a deferred artifact cleanup must not invalidate successful native shader compilation");
			ShaderCacheTestAccess::TakeManifestWriteCount(cache);
			if (compute)
			{
				auto retry = compiler->CompileAllPermutations(uid);
				Require(static_cast<bool>(retry), "an unchanged shader must schedule pending cache maintenance");
				retry->Wait();
				Require(retry->GetResult(), "cleanup-only compiler retry must succeed without recompilation");
			}
			else
			{
				Require(cache.SaveCache(), "graphics cache maintenance must retry deferred cleanup");
			}
			Require(!cache.NeedsMaintenance() && ShaderCacheTestAccess::TakeManifestWriteCount(cache) == 0 &&
				ShaderCacheTestAccess::GetGeneration(cache, uid, 0) == repairedGeneration,
				"native cleanup-only retry must reuse the compiled generation without rewriting metadata");
			ShaderCacheTestAccess::FailNextSaveAfterPublish(cache);
			Require(!cache.SaveCache(true) && cache.IsDirty() && shader->IsReady(),
				"unconfirmed manifest sync must leave the native shader usable and request persistence retry");
			ShaderCacheTestAccess::TakeManifestWriteCount(cache);
			if (compute)
			{
				auto retry = compiler->CompileAllPermutations(uid);
				Require(static_cast<bool>(retry), "published metadata with pending sync must schedule maintenance");
				retry->Wait();
				Require(retry->GetResult(), "native persistence retry must confirm sync without recompiling");
			}
			else
			{
				Require(cache.SaveCache(), "graphics metadata must retry unconfirmed sync");
			}
			Require(!cache.NeedsMaintenance() && shader->IsReady() &&
				ShaderCacheTestAccess::TakeManifestWriteCount(cache) == 1 &&
				ShaderCacheTestAccess::GetGeneration(cache, uid, 0) == repairedGeneration,
				"native sync retry must commit the same compiled generation before cleanup");
			ShaderCacheTestAccess::TakeArtifactReadCount(cache);
			Require(update() && shader->IsReady(), "the repaired shader must remain ready on a warm update");
			Require(ShaderCacheTestAccess::TakeArtifactReadCount(cache) == expectedReads &&
				ShaderCacheTestAccess::GetGeneration(cache, uid, 0) == repairedGeneration,
				"the repaired real shader must return to a one-read cache hit");
			auto stages = [&]()
			{
				return std::array<RHI::RHIShaderPtr, 6>{ shader->GetVertexShaderRHI(), shader->GetFragmentShaderRHI(),
					shader->GetComputeShaderRHI(), shader->GetDebugVertexShaderRHI(),
					shader->GetDebugFragmentShaderRHI(), shader->GetDebugComputeShaderRHI() };
			};
			const auto previousStages = stages();
			WriteShader(workspace, compute ? "WarmCompute" : "WarmGraphics", false, compute);
			Require(!App::UpdateAsset(uid.ToString().c_str()), "an invalid shader edit must report failure");
			Drain();
			Require(!update() && shader->IsReady() && stages() == previousStages,
				"a failed permutation compile must preserve every last-good RHI stage");
			WriteShader(workspace, compute ? "WarmCompute" : "WarmGraphics", true, compute);
			Require(App::UpdateAsset(uid.ToString().c_str()), "a repaired shader source must reload");
			Drain();
			Require(update() && shader->IsReady(), "RHI updates must recover after a failed source edit");
			if (!compute)
			{
				const auto retainedGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 0);
				ShaderCacheTestAccess::TakeManifestWriteCount(cache);
				auto all = compiler->CompileAllPermutations(uid);
				Require(static_cast<bool>(all), "remaining graphics permutations must schedule compilation");
				all->Wait();
				Drain();
				Require(all->GetResult() && ShaderCacheTestAccess::TakeManifestWriteCount(cache) == 1 &&
					ShaderCacheTestAccess::GetGeneration(cache, uid, 0) == retainedGeneration,
					"parallel compilation must commit once after every job without recompiling a warm permutation");
				for (uint32_t permutation = 0; permutation < 4; ++permutation)
				{
					Require(!cache.IsExpired(uid, permutation), "all requested permutations must survive the shared commit");
				}
			}
			std::cout << "Warm " << (compute ? "compute" : "graphics") << " RHI shader: " << reads
				<< " artifact reads; repair, reuse, deferred cleanup, sync retry and last-good preservation passed\n";
		}
	}

	void TestSharedShaderReload(const std::filesystem::path& workspace)
	{
		auto* compiler = App::GetSubmodule<ShaderCompiler>();
		auto& cache = ShaderCompilerTestAccess::GetShaderCache(*compiler);
		const auto includePath = workspace / "Content" / "BatchReload.glsl";
		auto writeInclude = [&](const char* source)
		{
			std::ofstream output(includePath);
			output << source;
			output.close();
			Require(static_cast<bool>(output), "shared shader include must be written");
		};
		writeInclude("const float SharedReloadValue = 0.125;\n");
		const FileId includeId = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(includePath.string());
		Require(static_cast<bool>(includeId),
			"the shared include must be registered in the active Content mount");
		std::array<FileId, 6> ids;
		std::array<ShaderSetPtr, 6> shaders;
		std::array<std::string, 6> generations;
		std::array<RHI::RHIShaderPtr, 6> stages;
		std::array<RHI::RHIShaderPtr, 6> debugStages;
		for (size_t i = 0; i < ids.size(); ++i)
		{
			const auto name = "BatchReload" + std::to_string(i);
			ids[i] = WriteShader(workspace, name.c_str(), true, true, i < 4 ? "BatchReload.glsl" : nullptr);
			Require(compiler->LoadShader_Immediate(ids[i], shaders[i]) && shaders[i] && shaders[i]->IsReady(),
				"shared shader fixtures must compile their regular and debug RHI stages");
			generations[i] = ShaderCacheTestAccess::GetGeneration(cache, ids[i], 0);
			stages[i] = shaders[i]->GetComputeShaderRHI();
			debugStages[i] = shaders[i]->GetDebugComputeShaderRHI();
		}
		Drain();
		Require(cache.SaveCache(), "the initial shared shader fixtures must commit");
		ShaderCacheTestAccess::TakeManifestWriteCount(cache);
		writeInclude("const float SharedReloadValue = 0.75;\n");
		auto reload = compiler->OnEffectiveContentChanged("BatchReload.glsl");
		Require(static_cast<bool>(reload), "a shared include edit must schedule dependent reloads");
		reload->Wait();
		Drain();
		Require(reload->GetResult(), "all dependent shaders must reload successfully");
		const auto writes = ShaderCacheTestAccess::TakeManifestWriteCount(cache);
		std::cout << "Shared include reload: " << writes << " manifest writes for 4 changed shaders of 6\n";
		Require(writes == 2, "one include reload must commit one invalidation and one completed shader batch");
		for (size_t i = 0; i < ids.size(); ++i)
		{
			Require(shaders[i]->IsReady(), "all shaders must retain complete regular/debug stages");
			const bool changed = ShaderCacheTestAccess::GetGeneration(cache, ids[i], 0) != generations[i];
			const bool replaced = shaders[i]->GetComputeShaderRHI() != stages[i];
			Require(changed == (i < 4) && replaced == (i < 4),
				"only dependent shaders must recompile and publish new RHI stages");
		}
		auto reloadInclude = [&]()
		{
			auto task = compiler->OnEffectiveContentChanged("BatchReload.glsl");
			Require(static_cast<bool>(task), "shared include reload must return its completion task");
			task->Wait();
			Drain();
			return task->GetResult();
		};
		auto rememberStages = [&]()
		{
			for (size_t i = 0; i < ids.size(); ++i)
			{
				generations[i] = ShaderCacheTestAccess::GetGeneration(cache, ids[i], 0);
				stages[i] = shaders[i]->GetComputeShaderRHI();
				debugStages[i] = shaders[i]->GetDebugComputeShaderRHI();
			}
		};
		auto requirePreviousStages = [&]()
		{
			for (size_t i = 0; i < ids.size(); ++i)
			{
				Require(shaders[i]->IsReady() && shaders[i]->GetComputeShaderRHI() == stages[i] &&
					shaders[i]->GetDebugComputeShaderRHI() == debugStages[i],
					"a failed batch must retain all last-good RHI stages");
			}
		};
		rememberStages();
		writeInclude("const float SharedReloadValue = 0.5;\n");
		ShaderCacheTestAccess::FailNextSaveBeforeReplace(cache);
		Require(!reloadInclude() && cache.IsDirty(), "failed invalidation must abort shader reload and retain retry state");
		requirePreviousStages();
		for (size_t i = 0; i < ids.size(); ++i)
		{
			Require(ShaderCacheTestAccess::GetGeneration(cache, ids[i], 0) == generations[i],
				"compilation must not start before the invalidation checkpoint succeeds");
		}
		Require(reloadInclude(), "a failed invalidation must be retryable");
		rememberStages();
		writeInclude("this is invalid GLSL\n");
		Require(!reloadInclude(), "failed shared include compilation must fail the batch");
		requirePreviousStages();
		writeInclude("const float SharedReloadValue = 0.25;\n");
		ShaderCacheTestAccess::FailNextSaveBeforeReplace(cache);
		Require(!reloadInclude() && cache.IsDirty(), "failed completion commit must retain persistence retry state");
		requirePreviousStages();
		Require(reloadInclude(), "repaired shader batch must recover from a failed completion commit");
		std::cout << "Shared include reload: invalidation, compile, commit failures and last-good recovery passed\n";

		auto* registry = App::GetSubmodule<AssetRegistry>();
		Require(registry->ScanContentFolder(), "seed the complete registry scan before measuring reload writes");
		Drain();
		Require(registry->CompleteScanProcessing(), "initial native scan must finish its acknowledgements");
		rememberStages();
		for (size_t i = 0; i < 3; ++i)
		{
			const auto path = workspace / "Content" / ("BatchReload" + std::to_string(i) + ".shader");
			std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(2));
		}
		registry->TakeManifestWritesForTests();
		ShaderCacheTestAccess::TakeManifestWriteCount(cache);
		Require(registry->ScanContentFolder(), "changed native shaders must publish the next registry generation");
		Drain();
		Require(registry->CompleteScanProcessing(), "native scan must join shader acknowledgements before committing");
		const uint64_t assetWrites = registry->TakeManifestWritesForTests();
		std::cout << "Asset registry scan: " << assetWrites << " manifest writes for 3 changed native shaders of 6\n";
		Require(assetWrites == 2, "native shader scan must use two asset-registry checkpoints");
		const uint64_t shaderWrites = ShaderCacheTestAccess::TakeManifestWriteCount(cache);
		std::cout << "Shader cache scan: " << shaderWrites << " manifest writes for 3 changed native shaders of 6\n";
		Require(shaderWrites == 2, "independent shader edits in one scan must share two shader-cache checkpoints");
		for (size_t i = 0; i < ids.size(); ++i)
		{
			Require(shaders[i]->IsReady() && !registry->IsAssetExpired(registry->GetAssetInfoPtr(ids[i])),
				"completed native shader results must have ready RHI stages and acknowledged asset revisions");
			Require((ShaderCacheTestAccess::GetGeneration(cache, ids[i], 0) != generations[i]) == (i < 3),
				"the native scan must only recompile changed shader fixtures");
		}

		auto scan = [&]()
		{
			const bool bScanned = registry->ScanContentFolder();
			Drain();
			return registry->CompleteScanProcessing() && bScanned;
		};
		auto editShader = [&](size_t index, bool valid)
		{
			const auto path = workspace / "Content" / ("BatchReload" + std::to_string(index) + ".shader");
			auto yaml = YAML::LoadFile(path.string());
			yaml["glslCompute"] = valid ? "layout(local_size_x = 1) in; void main() {}" : "invalid GLSL";
			std::ofstream output(path);
			output << yaml;
			output.close();
			Require(static_cast<bool>(output), "scan shader edit must be written");
			std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(2));
		};
		struct RestoreLazyLoading
		{
			bool m_previous = g_bUseLazyAssetInfoLoading;
			~RestoreLazyLoading() { g_bUseLazyAssetInfoLoading = m_previous; }
		} restoreLazyLoading;
		for (bool lazy : { false, true })
		{
			g_bUseLazyAssetInfoLoading = lazy;
			rememberStages();
			writeInclude("const float SharedReloadValue = 0.375;\n");
			for (size_t i = 0; i < 3; ++i) editShader(i, true);
			ShaderCacheTestAccess::TakeManifestWriteCount(cache);
			registry->TakeManifestWritesForTests();
			Require(scan(), "overlapping include/direct changes must reload successfully");
			Require(ShaderCacheTestAccess::TakeManifestWriteCount(cache) == 2 && registry->TakeManifestWritesForTests() == 2,
				"overlapping include/direct changes must share both cache checkpoints");
			for (size_t i = 0; i < ids.size(); ++i)
			{
				Require((ShaderCacheTestAccess::GetGeneration(cache, ids[i], 0) != generations[i]) == (i < 4),
					"include/direct changes must only replace their deduplicated dependents");
			}

			rememberStages();
			editShader(0, false);
			editShader(4, true);
			writeInclude("const float SharedReloadValue = 0.625;\n");
			Require(!scan(), "one broken shader must fail the affected scan result");
			Require(shaders[0]->IsReady() && shaders[0]->GetComputeShaderRHI() == stages[0],
				"the broken shader must keep its last-good RHI stage");
			Require(registry->IsAssetExpired(registry->GetAssetInfoPtr(ids[0])) &&
				registry->IsAssetExpired(registry->GetAssetInfoPtr(includeId)) &&
				!registry->IsAssetExpired(registry->GetAssetInfoPtr(ids[4])) &&
				shaders[4]->GetComputeShaderRHI() != stages[4],
				"failed source/include acknowledgements must not reject an independent successful shader");
			rememberStages();
			editShader(0, true);
			Require(scan(), "repair must retry the failed source and include");
			Require(!registry->IsAssetExpired(registry->GetAssetInfoPtr(includeId)) &&
				ShaderCacheTestAccess::GetGeneration(cache, ids[4], 0) == generations[4],
				"repair must not recompile the unrelated successful shader");

			rememberStages();
			for (size_t i = 0; i < 3; ++i) editShader(i, true);
			ShaderCacheTestAccess::FailNextSaveBeforeReplace(cache);
			Require(!scan(), "failed shader invalidation must fail the entire requested batch");
			requirePreviousStages();
			for (size_t i = 0; i < ids.size(); ++i)
			{
				Require(ShaderCacheTestAccess::GetGeneration(cache, ids[i], 0) == generations[i],
					"a rejected invalidation must not compile any changed shader");
			}
			Require(scan(), "a rejected invalidation must retry with a nonempty healthy cache");

			rememberStages();
			for (size_t i = 0; i < 3; ++i) editShader(i, true);
			ShaderCacheTestAccess::FailNextSaveBeforeReplace(cache, 1);
			Require(!scan(), "failed shader completion persistence must fail the batch");
			requirePreviousStages();
			Require(scan(), "failed completion persistence must retry without losing last-good resources");
			rememberStages();
			Require(std::filesystem::remove(includePath), "remove only the fixture-owned include source");
			Require(!scan(), "removing an effective include must fail its dependent reloads");
			requirePreviousStages();
			Require(!scan(), "unchanged missing include must remain retryable, not turn into a successful scan");
			writeInclude("const float SharedReloadValue = 0.875;\n");
			Require(scan(), "restoring the effective include must recover failed dependents");
			rememberStages();
			ShaderCacheTestAccess::TakeManifestWriteCount(cache);
			registry->TakeManifestWritesForTests();
			Require(scan() && ShaderCacheTestAccess::TakeManifestWriteCount(cache) == 0 &&
				registry->TakeManifestWritesForTests() == 0,
				"an unchanged scan after recovery must not repeat successful work or rewrite either manifest");
			requirePreviousStages();
			std::cout << "Shader scan batch: " << (lazy ? "lazy" : "eager")
				<< " overlap, isolated failures and checkpoint retry passed\n";
		}
	}

	class HoldTextureDecode
	{
	public:
		explicit HoldTextureDecode(FileId id) : m_id(id)
		{
			Drain();
			s_active = this;
			m_original = TextureImporterTestAccess::ExchangeDecoder(*App::GetSubmodule<TextureImporter>(), &Decode);
		}
		~HoldTextureDecode()
		{
			Release();
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(
				{ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
			TextureImporterTestAccess::ExchangeDecoder(*App::GetSubmodule<TextureImporter>(), m_original);
			s_active = nullptr;
		}
		void Wait()
		{
			const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (!m_decoded.load() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
			Require(m_decoded.load(), "controlled texture decode must start");
		}
		void Release()
		{
			if (!m_released.exchange(true)) m_release.count_down();
		}
	private:
		static bool Decode(const TextureImporter::CpuDecodeRequest& request, TextureImporter::ByteCode& bytes,
			int32_t& width, int32_t& height, uint32_t& mips)
		{
			const bool result = TextureImporter::DecodeTextureCpu(request, bytes, width, height, mips);
			if (request.m_fileId == s_active->m_id && !s_active->m_decoded.exchange(true))
			{
				s_active->m_release.wait();
			}
			return result;
		}
		inline static HoldTextureDecode* s_active = nullptr;
		FileId m_id;
		TextureImporterTestAccess::Decoder m_original;
		std::atomic<bool> m_decoded{ false }, m_released{ false };
		std::latch m_release{ 1 };
	};

	class ReloadObserver final : public Object
	{
	public:
		ReloadObserver(MaterialPtr material) : m_material(material) {}
		Tasks::ITaskPtr OnHotReload() override
		{
			++m_notifications;
			m_thread = App::GetSubmodule<Tasks::Scheduler>()->GetCurrentThreadType();
			m_revision = m_material->GetContentRevision();
			return {};
		}
		MaterialPtr m_material;
		uint32_t m_notifications = 0;
		uint64_t m_revision = 0;
		EThreadType m_thread = EThreadType::Main;
	};

	class ObserveReload
	{
	public:
		explicit ObserveReload(MaterialPtr material) : m_material(material),
			m_observer(TObjectPtr<ReloadObserver>::Make(App::GetSubmodule<MaterialImporter>()->GetAllocator(), material))
		{
			material->AddHotReloadDependentObject(m_observer);
		}
		~ObserveReload()
		{
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(
				{ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
			m_material->RemoveHotReloadDependentObject(m_observer);
			m_observer.DestroyObject(App::GetSubmodule<MaterialImporter>()->GetAllocator());
		}
		MaterialPtr m_material;
		TObjectPtr<ReloadObserver> m_observer;
	};

	void TestFailedTextureReload(const std::filesystem::path& workspace)
	{
		const auto red = WriteTexture(workspace, "MaterialRed", true);
		const auto invalid = WriteTexture(workspace, "MaterialInvalid", false);
		MaterialFixture fixture(workspace, "FailedTextureMaterial", red);
		const auto material = fixture.Load();
		const auto bindings = material->GetShaderBindings();
		const auto bytes = ReadGpu(bindings);
		ObserveReload observed(material);
		const auto revision = material->GetContentRevision();
		const auto metadataRevision = material->GetRenderMetadataRevision();
		TexturePtr originalTexture;
		Require(material->GetSamplers().TryGet("baseColorSampler", originalTexture), "original sampler must exist");
		auto document = fixture.Read();
		auto samplers = document["samplers"].as<TMap<std::string, FileId>>();
		samplers["baseColorSampler"] = invalid;
		auto uniforms = document["uniformsVec4"].as<TMap<std::string, glm::vec4>>();
		uniforms["material.baseColorFactor"] = glm::vec4(0, 1, 0, 0.5f);
		document["samplers"] = samplers;
		document["uniformsVec4"] = uniforms;
		fixture.Write(document);
		fixture.Reload();
		Drain();
		glm::vec4 color;
		TexturePtr texture;
		Require(material->GetShaderBindings() == bindings &&
			material->GetContentRevision() == revision &&
			material->GetRenderMetadataRevision() == metadataRevision &&
			material->GetUniformsVec4().TryGet("material.baseColorFactor", color) && color == glm::vec4(1, 0.5f, 0.25f, 1) &&
			material->GetSamplers().TryGet("baseColorSampler", texture) && texture == originalTexture,
			"failed texture reload must preserve the complete last-good material and bindings");
		Require(ReadGpu(bindings) == bytes && observed.m_observer->m_notifications == 0,
			"failed dependency must not change retained GPU data or notify dependants");
		auto* importer = App::GetSubmodule<MaterialImporter>();
		Require(!importer->GetLoadPromise(fixture.id)->GetResult(), "failed reload task must report failure");
		const auto previousGlobal = Material::GetGlobalContentRevision();
		Require(WriteTexture(workspace, "MaterialInvalid", true) == invalid, "texture repair must retain FileId");
		fixture.Reload();
		Drain();
		Require(importer->GetLoadPromise(fixture.id)->GetResult() == material &&
			Color(material) == glm::vec4(0, 1, 0, 0.5f) && material->GetContentRevision() == revision + 1 &&
			Material::GetGlobalContentRevision() == previousGlobal + 1 &&
			observed.m_observer->m_notifications == 1 && observed.m_observer->m_revision == revision + 1 &&
			observed.m_observer->m_thread == EThreadType::Render,
			"successful retry must publish once on Render and notify dependants after commit");
		CheckGpuColor(material->GetShaderBindings(), ReadGpu(material->GetShaderBindings()), Color(material));
		Require(ReadGpu(bindings) == bytes, "accepted retry must retain previous GPU binding contents");
	}

	void TestColdFailureRetry(const std::filesystem::path& workspace)
	{
		auto* importer = App::GetSubmodule<MaterialImporter>();
		MaterialPtr missing;
		Require(!importer->LoadMaterial_Immediate(FileId::CreateNewFileId(), missing) && !missing,
			"an unknown material must fail without dereferencing an absent task");
		const auto image = WriteTexture(workspace, "ColdMaterialInvalid", false);
		MaterialFixture fixture(workspace, "ColdMaterialFailure", image);
		MaterialPtr material;
		Require(!importer->LoadMaterial_Immediate(fixture.id, material) && material && !material->GetShaderBindings(),
			"failed cold dependency must not publish a usable material or report success");
		const auto original = material;
		Require(WriteTexture(workspace, "ColdMaterialInvalid", true) == image, "cold repair must preserve texture identity");
		Require(importer->LoadMaterial_Immediate(fixture.id, material) && material == original,
			"completed cold failure must retry on the existing Material");
		Drain();
		Require(material->IsReady() && material->GetContentRevision() == 1, "cold retry must publish once");
	}

	void TestOrderedReload(const std::filesystem::path& workspace, bool cold)
	{
		auto* importer = App::GetSubmodule<MaterialImporter>();
		const auto originalImage = WriteTexture(workspace, cold ? "ColdOrderOld" : "ReloadOrderOld", true);
		const auto pendingImage = WriteTexture(workspace, cold ? "ColdOrderNext" : "ReloadOrderNext", true);
		MaterialFixture fixture(workspace, cold ? "ColdMaterialOrder" : "MaterialReloadOrder",
			cold ? pendingImage : originalImage);
		auto document = fixture.Read();
		MaterialPtr material;
		RHI::RHIShaderBindingSetPtr originalBindings;
		TVector<uint8_t> originalBytes;
		uint64_t revision = 0;
		if (!cold)
		{
			material = fixture.Load();
			originalBindings = material->GetShaderBindings();
			originalBytes = ReadGpu(originalBindings);
			revision = material->GetContentRevision();
		}
		HoldTextureDecode hold(pendingImage);
		Tasks::TaskPtr<MaterialPtr> first;
		if (cold)
		{
			first = importer->LoadMaterial(fixture.id, material);
		}
		else
		{
			auto samplers = document["samplers"].as<TMap<std::string, FileId>>();
			samplers["baseColorSampler"] = pendingImage;
			document["samplers"] = samplers;
			SetColor(document, glm::vec4(0, 1, 0, 1));
			fixture.Write(document);
			fixture.Reload();
			first = importer->GetLoadPromise(fixture.id);
		}
		hold.Wait();
		Require(!first->IsFinished() && (cold ? !material->GetShaderBindings() :
			material->GetShaderBindings() == originalBindings && Color(material) == glm::vec4(1, 0.5f, 0.25f, 1)),
			"pending dependencies must leave the live material untouched");
		{
			std::ofstream invalid(fixture.path);
			invalid << "[";
		}
		fixture.Reload();
		Require(importer->GetLoadPromise(fixture.id) == first, "parse failure must keep the existing publication chain");
		SetColor(document, glm::vec4(0.25f, 0, 1, 0.75f));
		fixture.Write(document);
		fixture.Reload();
		auto last = importer->GetLoadPromise(fixture.id);
		MaterialPtr duplicate;
		Require(last && last != first && importer->LoadMaterial(fixture.id, duplicate) == last && duplicate == material,
			"cold and reload callers must share the latest pending publication");
		std::jthread collect([&](std::stop_token stop)
			{
				while (!stop.stop_requested())
				{
					importer->CollectGarbage();
					std::this_thread::yield();
				}
			});
		for (uint32_t i = 0; i < 16; ++i)
		{
			SetColor(document, glm::vec4(float(i), 0.25f, 0.5f, 1));
			fixture.Write(document);
			fixture.Reload();
		}
		last = importer->GetLoadPromise(fixture.id);
		Require(last && !last->IsFinished(), "promise GC must retain a blocked final material reload");
		collect.request_stop();
		collect.join();
		hold.Release();
		last->Wait();
		Drain();
		Require(last->GetResult() == material && first->IsFinished() &&
			importer->GetLoadedMaterial(fixture.id) == material && Color(material) == glm::vec4(15, 0.25f, 0.5f, 1) &&
			material->GetContentRevision() == revision + 18,
			"ordered material publication must retain identity and install the last authored request exactly once");
		CheckGpuColor(material->GetShaderBindings(), ReadGpu(material->GetShaderBindings()), Color(material));
		if (!cold)
		{
			Require(ReadGpu(originalBindings) == originalBytes, "repeated reloads must not mutate old GPU bindings");
			const auto current = material->GetContentRevision();
			auto oldTexture = App::GetSubmodule<TextureImporter>()->GetLoadedTexture(originalImage);
			oldTexture->TraceHotReload(nullptr);
			Drain();
			Require(material->GetContentRevision() == current, "removed sampler must no longer notify this material");
			auto newTexture = App::GetSubmodule<TextureImporter>()->GetLoadedTexture(pendingImage);
			newTexture->TraceHotReload(nullptr);
			Drain();
			Require(material->GetContentRevision() > current, "replacement sampler must retain dependency notifications");
		}
	}

	void TestShaderFailures(const std::filesystem::path& workspace)
	{
		auto* importer = App::GetSubmodule<MaterialImporter>();
		const auto validShader = WriteShader(workspace, "MaterialShaderGood");
		const auto invalidShader = WriteShader(workspace, "MaterialShaderInvalid", false);
		const auto computeShader = WriteShader(workspace, "MaterialComputeOnly", true, true);
		MaterialFixture fixture(workspace, "ShaderFailureMaterial", {}, validShader);
		auto material = fixture.Load();
		const auto bindings = material->GetShaderBindings();
		const auto bytes = ReadGpu(bindings);
		const auto revision = material->GetContentRevision();
		ObserveReload observed(material);
		auto document = fixture.Read();
		for (auto shader : { invalidShader, computeShader, FileId::CreateNewFileId() })
		{
			document["shaderUid"] = shader;
			SetColor(document, glm::vec4(0, 0, 1, 1));
			fixture.Write(document);
			fixture.Reload();
			Drain();
			auto task = importer->GetLoadPromise(fixture.id);
			Require(task && !task->GetResult() && material->GetShaderBindings() == bindings &&
				material->GetContentRevision() == revision && ReadGpu(bindings) == bytes &&
				observed.m_observer->m_notifications == 0,
				"failed shader lookup, compilation or graphics creation must retain the complete old material");
		}
		document["shaderUid"] = validShader;
		fixture.Write(document);
		fixture.Reload();
		Drain();
		Require(importer->GetLoadPromise(fixture.id)->GetResult() == material &&
			material->GetContentRevision() == revision + 1 && observed.m_observer->m_notifications == 1,
			"valid shader retry must recover after dependency and graphics creation failures");
		CheckGpuColor(material->GetShaderBindings(), ReadGpu(material->GetShaderBindings()), glm::vec4(0, 0, 1, 1));
		Require(ReadGpu(bindings) == bytes, "shader retry must leave retained old GPU data unchanged");
	}

	void TestLayoutAndColdParity(const std::filesystem::path& workspace)
	{
		const auto shader = WriteShader(workspace, "MaterialLayout");
		const auto image = WriteTexture(workspace, "MaterialLayoutImage", true);
		MaterialFixture fixture(workspace, "MaterialLayoutReload", image, shader);
		auto material = fixture.Load();
		const auto oldBindings = material->GetShaderBindings();
		const auto oldBytes = ReadGpu(oldBindings);
		const auto oldMetadata = material->GetRenderMetadataRevision();
		auto document = fixture.Read();
		SetColor(document, glm::vec4(0.125f, 0.25f, 0.75f, 0.5f));
		document["defines"] = TVector<std::string>{ "EXTRA" };
		document["renderQueue"] = "Transparent";
		document["blendMode"] = RHI::EBlendMode::AlphaBlending;
		document["cullMode"] = RHI::ECullMode::None;
		document["bEnableZWrite"] = false;
		document["bSupportMultisampling"] = false;
		auto floats = document["uniformsFloat"].as<TMap<std::string, float>>();
		floats["material.alphaCutoff"] = 0.375f;
		document["uniformsFloat"] = floats;
		fixture.Write(document);
		fixture.Reload();
		Drain();
		const auto newBindings = material->GetShaderBindings();
		const auto newBytes = ReadGpu(newBindings);
		Require(newBytes.Num() > oldBytes.Num() && material->GetRenderMetadataRevision() > oldMetadata &&
			material->GetShader()->GetDefines().Contains("MOTIONS") && material->GetShader()->GetDefines().Contains("EXTRA") &&
			material->GetRenderState().GetBlendMode() == RHI::EBlendMode::AlphaBlending &&
			material->GetRenderState().GetCullMode() == RHI::ECullMode::None &&
			!material->GetRenderState().IsEnabledZWrite() && !material->GetRenderState().SupportMultisampling(),
			"reload must replace reflected layout and preserve authored forward/render settings");
		CheckGpuColor(newBindings, newBytes, Color(material));
		Require(ReadGpu(oldBindings) == oldBytes, "layout replacement must retain old GPU allocation contents");
		MaterialFixture cold(workspace, "MaterialLayoutCold", image, shader);
		cold.Write(document);
		auto matching = cold.Load();
		Require(matching->GetShader()->GetDefines() == material->GetShader()->GetDefines(),
			"cold load and hot reload must resolve the same shader permutation");
		Require(ReadGpu(matching->GetShaderBindings()) == newBytes,
			"cold load and hot reload must produce identical GPU bytes");
		Require(matching->GetRenderState() == material->GetRenderState(),
			"identical cold/reload render states must compare equal regardless of object padding");
	}

	void TestLiveShaderEdit(const std::filesystem::path& workspace)
	{
		const auto shader = WriteShader(workspace, "MaterialLiveEdit");
		MaterialFixture fixture(workspace, "LiveShaderMaterial", {}, shader);
		auto material = fixture.Load();
		const auto bindings = material->GetShaderBindings();
		const auto bytes = ReadGpu(bindings);
		auto revision = material->GetContentRevision();
		const auto path = workspace / "Content/MaterialLiveEdit.shader";
		auto document = YAML::LoadFile(path.string());
		const auto originalFragment = document["glslFragment"].as<std::string>();
		document["glslFragment"] = "not valid GLSL";
		{
			std::ofstream output(path);
			output << document;
		}
		Require(!App::UpdateAsset(shader.ToString().c_str()), "invalid live shader edit must report compiler failure");
		Drain();
		Require(material->GetContentRevision() == revision && material->GetShaderBindings() == bindings &&
			ReadGpu(bindings) == bytes, "failed live shader compilation must not notify or destroy last-good material data");

		auto fragment = originalFragment;
		const auto offset = fragment.find("float roughnessFactor;");
		Require(offset != std::string::npos, "generated shader fixture must expose its layout edit point");
		fragment.insert(offset, "vec4 extraStorage;\n");
		document["glslFragment"] = fragment;
		{
			std::ofstream output(path);
			output << document;
		}
		const auto beforeTime = std::filesystem::last_write_time(path);
		const auto beforeSize = std::filesystem::file_size(path);
		Require(App::UpdateAsset(shader.ToString().c_str()), "repaired shader must reload");
		Drain();
		Require(beforeTime == std::filesystem::last_write_time(path) && beforeSize == std::filesystem::file_size(path),
			"shader acknowledgement must succeed without changing the source revision");
		const auto updated = material->GetShaderBindings();
		const auto updatedBytes = ReadGpu(updated);
		Require(material->GetContentRevision() > revision && updatedBytes.Num() > bytes.Num(),
			"live shader repair must propagate its reflected layout to dependent materials");
		CheckGpuColor(updated, updatedBytes, Color(material));
		Require(ReadGpu(bindings) == bytes, "shader layout edit must retain old material GPU data");
	}

	class InstanceWorld final : public World
	{
	public:
		InstanceWorld() : World("Material instance test", 0, {}) {}
		void SetCommandList(RHI::RHICommandListPtr command) { m_commandList = std::move(command); }
	};

	void TestPrivateInstance(const std::filesystem::path& workspace)
	{
		const auto shader = WriteShader(workspace, "MaterialPrivateInstance");
		const auto image = WriteTexture(workspace, "MaterialPrivateLayer", true);
		MaterialFixture fixture(workspace, "PrivateInstanceSource", {}, shader);
		auto source = fixture.Load();
		TexturePtr layer;
		Require(App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(image, layer), "private layer must load");
		InstanceWorld world;
		MaterialPtr instance;
		auto create = Tasks::CreateTask("Create private material instance", [&]()
			{
				using namespace RHI;
				auto command = Renderer::GetDriver()->CreateCommandList(false, ECommandListQueue::Transfer);
				Renderer::GetDriverCommands()->BeginCommandList(command, true);
				world.SetCommandList(command);
				instance = Material::CreateInstance(&world, source);
				instance->SetSampler("layer0Sampler", layer);
				Renderer::GetDriverCommands()->EndCommandList(command);
				auto fence = RHIFencePtr::Make();
				Require(Renderer::GetDriver()->SubmitCommandList(command, fence) &&
					fence->Wait(5000000000ull) == EFenceStatus::Finished, "private material initialization must upload");
				fence->ClearDependencies();
				world.SetCommandList({});
			}, EThreadType::Render);
		create->Run();
		create->Wait();
		Require(instance.IsValid(), "real private Material::CreateInstance must succeed");
		const auto identity = instance;
		const auto oldBindings = instance->GetShaderBindings();
		const auto oldBytes = ReadGpu(oldBindings);
		const auto metadata = source->GetRenderMetadataRevision();
		auto synchronize = Tasks::CreateTask("Synchronize private material values", [&]()
			{
				source->SetUniform("material.baseColorFactor", glm::vec4(0.5f, 0.75f, 0.125f, 1));
				instance->SynchronizeUniformValues(*source);
			}, EThreadType::Render);
		synchronize->Run();
		synchronize->Wait();
		Drain();
		TexturePtr retainedLayer;
		Require(instance == identity && source->GetRenderMetadataRevision() == metadata &&
			instance->GetSamplers().TryGet("layer0Sampler", retainedLayer) && retainedLayer == layer,
			"content-only synchronization must reuse a private instance without losing its layer samplers");
		CheckGpuColor(instance->GetShaderBindings(), ReadGpu(instance->GetShaderBindings()), Color(source));
		Require(ReadGpu(oldBindings) == oldBytes, "private synchronization must retain earlier GPU contents");
		auto destroy = Tasks::CreateTask("Destroy private material fixture", [&]()
			{
				instance.DestroyObject(world.GetAllocator());
			}, EThreadType::Render);
		destroy->Run();
		destroy->Wait();
	}
}

namespace Sailor::Tests
{
	void RunMaterialImporterCommandTests(const std::filesystem::path& workspace)
	{
		std::string failures;
		auto run = [&](const char* name, auto test)
		{
			try { test(); }
			catch (const std::exception& error) { failures += std::string(name) + ": " + error.what() + '\n'; }
		};
		run("Texture failure and retry", [&]() { TestFailedTextureReload(workspace); });
		run("Cold failure and retry", [&]() { TestColdFailureRetry(workspace); });
		run("Reload ordering", [&]() { TestOrderedReload(workspace, false); });
		run("Cold ordering", [&]() { TestOrderedReload(workspace, true); });
		run("Shader failures", [&]() { TestShaderFailures(workspace); });
		run("Warm shader permutation", [&]() { TestWarmShaderPermutation(workspace); });
		run("Shared shader reload", [&]() { TestSharedShaderReload(workspace); });
		run("Layout and cold parity", [&]() { TestLayoutAndColdParity(workspace); });
		run("Live shader edit", [&]() { TestLiveShaderEdit(workspace); });
		run("Private instance", [&]() { TestPrivateInstance(workspace); });
		if (!failures.empty()) throw std::runtime_error(failures);
		std::cout << "Material importer preparation, ordered publication, dependency retry and GPU parity tests passed\n";
	}
}
