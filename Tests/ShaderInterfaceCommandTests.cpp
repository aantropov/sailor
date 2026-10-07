#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "FrameGraph/BloomNode.h"
#include "FrameGraph/DepthPrepassNode.h"
#include "FrameGraph/RenderSceneNode.h"
#include "FrameGraph/ShadowPrepassNode.h"
#include "FrameGraph/SkyParameters.h"
#include "GraphicsDriver/Vulkan/VulkanShaderModule.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/GpuCulling.h"
#include "RHI/Lighting.h"
#include "RHI/Shader.h"
#include "Workspace/WorkspacePathEncoding.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <span>
#include <stdexcept>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

namespace
{
	class BloomLayout : public BloomNode
	{
	public:
		using BloomNode::PushConstantsDownscale;
		using BloomNode::PushConstantsUpscale;
	};

	struct Field
	{
		StringHash m_name;
		size_t m_offset;
		size_t m_size;
	};

#define FIELD(type, field, name) Field{ name, offsetof(type, field), sizeof(type::field) }
	const std::array FrameFields{
		FIELD(UboFrameData, m_view, "view"_h),
		FIELD(UboFrameData, m_projection, "projection"_h),
		FIELD(UboFrameData, m_invProjection, "invProjection"_h),
		FIELD(UboFrameData, m_cameraPosition, "cameraPosition"_h),
		FIELD(UboFrameData, m_viewportSize, "viewportSize"_h),
		FIELD(UboFrameData, m_cameraZNearZFar, "cameraZNearZFar"_h),
		FIELD(UboFrameData, m_currentTime, "currentTime"_h),
		FIELD(UboFrameData, m_deltaTime, "deltaTime"_h)
	};
	using MainInstance = RenderSceneNode::PerInstanceData;
	const std::array MainFields{
		FIELD(MainInstance, model, "model"_h),
		FIELD(MainInstance, sphereBounds, "sphereBounds"_h),
		FIELD(MainInstance, materialInstance, "materialInstance"_h),
		FIELD(MainInstance, skeletonOffset, "skeletonOffset"_h),
		FIELD(MainInstance, bIsCulled, "isCulled"_h),
		FIELD(MainInstance, padding, "padding"_h),
		FIELD(MainInstance, bakedVolumeScale, "bakedVolumeScale"_h),
		FIELD(MainInstance, motion, "motion"_h)
	};
	const std::array MotionFields{
		FIELD(RHIObjectMotionData, m_previousModel, "previousModel"_h),
		FIELD(RHIObjectMotionData, m_state, "state"_h)
	};
	const std::array BoneFields{ Field{ "matrix"_h, 0, sizeof(glm::mat4) } };
	using DepthInstance = DepthPrepassNode::PerInstanceData;
	const std::array DepthFields{
		FIELD(DepthInstance, model, "model"_h),
		FIELD(DepthInstance, sphereBounds, "sphereBounds"_h),
		FIELD(DepthInstance, materialInstance, "materialInstance"_h),
		FIELD(DepthInstance, skeletonOffset, "skeletonOffset"_h),
		FIELD(DepthInstance, padding, "padding"_h),
		FIELD(DepthInstance, reserved, "reserved"_h)
	};
	using ShadowInstance = ShadowPrepassNode::PerInstanceData;
	const std::array ShadowFields{
		FIELD(ShadowInstance, model, "model"_h),
		FIELD(ShadowInstance, sphereBounds, "sphereBounds"_h),
		FIELD(ShadowInstance, materialInstance, "materialInstance"_h),
		FIELD(ShadowInstance, skeletonOffset, "skeletonOffset"_h),
		FIELD(ShadowInstance, bIsCulled, "isCulled"_h),
		FIELD(ShadowInstance, padding, "padding"_h),
		FIELD(ShadowInstance, bakedVolumeScale, "bakedVolumeScale"_h),
		FIELD(ShadowInstance, baseColorAlpha, "baseColorAlpha"_h),
		FIELD(ShadowInstance, baseColorSampler, "baseColorSampler"_h),
		FIELD(ShadowInstance, alphaCutoff, "alphaCutoff"_h),
		FIELD(ShadowInstance, maskedPadding, "maskedPadding"_h)
	};
	const std::array SkyFields{
		FIELD(SkyParameters, m_lightDirection, "lightDirection"_h),
		FIELD(SkyParameters, m_sunIlluminance, "sunIlluminance"_h),
		FIELD(SkyParameters, m_groundRadiance, "groundRadiance"_h),
		FIELD(SkyParameters, m_cloudsAttenuation1, "cloudsAttenuation1"_h),
		FIELD(SkyParameters, m_cloudsAttenuation2, "cloudsAttenuation2"_h),
		FIELD(SkyParameters, m_cloudsDensity, "cloudsDensity"_h),
		FIELD(SkyParameters, m_cloudsCoverage, "cloudsCoverage"_h),
		FIELD(SkyParameters, m_phaseInfluence1, "phaseInfluence1"_h),
		FIELD(SkyParameters, m_phaseInfluence2, "phaseInfluence2"_h),
		FIELD(SkyParameters, m_eccentrisy1, "eccentrisy1"_h),
		FIELD(SkyParameters, m_eccentrisy2, "eccentrisy2"_h),
		FIELD(SkyParameters, m_fog, "fog"_h),
		FIELD(SkyParameters, m_cloudScatteringScale, "cloudScatteringScale"_h),
		FIELD(SkyParameters, m_ambient, "ambient"_h),
		FIELD(SkyParameters, m_scatteringSteps, "scatteringSteps"_h),
		FIELD(SkyParameters, m_scatteringDensity, "scatteringDensity"_h),
		FIELD(SkyParameters, m_scatteringIntensity, "scatteringIntensity"_h),
		FIELD(SkyParameters, m_scatteringPhase, "scatteringPhase"_h),
		FIELD(SkyParameters, m_sunShaftsIntensity, "sunShaftsIntensity"_h),
		FIELD(SkyParameters, m_sunShaftsDistance, "sunShaftsDistance"_h)
	};
	const std::array LightFields{
		FIELD(RHILightShaderData, m_type, "type"_h),
		FIELD(RHILightShaderData, m_shadowType, "shadowType"_h),
		FIELD(RHILightShaderData, m_activeCascadeCount, "activeCascadeCount"_h),
		FIELD(RHILightShaderData, m_shadowBias, "shadowBias"_h),
		FIELD(RHILightShaderData, m_worldPosition, "worldPosition"_h),
		FIELD(RHILightShaderData, m_shadowDistance, "shadowDistance"_h),
		FIELD(RHILightShaderData, m_direction, "direction"_h),
		FIELD(RHILightShaderData, m_intensity, "intensity"_h),
		FIELD(RHILightShaderData, m_cutOff, "cutOff"_h),
		FIELD(RHILightShaderData, m_bounds, "bounds"_h)
	};
	const std::array IndirectFields{
		FIELD(DrawIndexedIndirectData, m_indexCount, "indexCount"_h),
		FIELD(DrawIndexedIndirectData, m_instanceCount, "instanceCount"_h),
		FIELD(DrawIndexedIndirectData, m_firstIndex, "firstIndex"_h),
		FIELD(DrawIndexedIndirectData, m_vertexOffset, "vertexOffset"_h),
		FIELD(DrawIndexedIndirectData, m_firstInstance, "firstInstance"_h)
	};
#undef FIELD

	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	class SpirvReflection
	{
	public:
		explicit SpirvReflection(RHIShaderPtr shader)
		{
			Require(shader.IsValid(), "required shader stage is absent");
			const auto& code = shader->m_vulkan.m_shader->m_module->m_byteCode;
			Require(spvReflectCreateShaderModule(code.Num() * sizeof(uint32_t), code.GetData(), &m_module) ==
				SPV_REFLECT_RESULT_SUCCESS, "compiled shader must reflect");
		}
		~SpirvReflection() { spvReflectDestroyShaderModule(&m_module); }
		SpirvReflection(const SpirvReflection&) = delete;
		SpirvReflection& operator=(const SpirvReflection&) = delete;

		const SpvReflectBlockVariable& Block(uint32_t set, uint32_t binding, bool array, size_t size) const
		{
			SpvReflectResult result;
			const auto* descriptor = spvReflectGetDescriptorBinding(&m_module, binding, set, &result);
			if (result != SPV_REFLECT_RESULT_SUCCESS || !descriptor)
			{
				std::string actual;
				for (uint32_t i = 0; i < m_module.descriptor_binding_count; ++i)
					actual += " " + std::to_string(m_module.descriptor_bindings[i].set) + ":" +
						std::to_string(m_module.descriptor_bindings[i].binding);
				throw std::runtime_error("missing descriptor " + std::to_string(set) + ":" + std::to_string(binding) +
					"; compiled bindings:" + actual);
			}
			Require(descriptor->descriptor_type == (array ? SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER :
				SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER), "compiled descriptor type differs from its C++ upload path");
			if (!array)
			{
				Require(descriptor->block.padded_size == (size + 15) / 16 * 16,
					"compiled uniform block extent differs from its aligned C++ payload");
				return descriptor->block;
			}
			Require(descriptor->block.member_count == 1, "per-instance storage must contain one array");
			const auto& instances = descriptor->block.members[0];
			const auto& arrayType = instances.type_description->traits.array;
			Require(arrayType.dims_count == 1 && arrayType.dims[0] == 0 && arrayType.stride == size,
				"compiled runtime-array stride differs from sizeof the C++ record");
			return instances;
		}

		const SpvReflectBlockVariable& PushConstants(size_t size) const
		{
			SpvReflectResult result;
			const auto* block = spvReflectGetPushConstantBlock(&m_module, 0, &result);
			Require(result == SPV_REFLECT_RESULT_SUCCESS && block,
				"compiled push-constant block must exist");
			Require(block->offset == 0 && block->size <= size && size % 4 == 0,
				"C++ push-constant payload must cover the compiled range");
			return *block;
		}

	private:
		SpvReflectShaderModule m_module{};
	};

	const SpvReflectBlockVariable& Member(const SpvReflectBlockVariable& block, const Field& field, bool names)
	{
		for (uint32_t i = 0; i < block.member_count; ++i)
		{
			const auto& member = block.members[i];
			if (names ? (member.name && StringHash::Runtime(member.name) == field.m_name) : member.offset == field.m_offset)
				return member;
		}
		throw std::runtime_error("compiled member missing: " + field.m_name.ToString());
	}

	void CheckFields(const SpvReflectBlockVariable& block, std::span<const Field> fields, bool names)
	{
		Require(block.member_count == fields.size(), "compiled member count differs from the C++ field mapping");
		for (const auto& field : fields)
		{
			const auto& member = Member(block, field, names);
			if (member.offset != field.m_offset || member.size != field.m_size)
				throw std::runtime_error("compiled offset/size differs for " + field.m_name.ToString() +
					": " + std::to_string(member.offset) + "/" + std::to_string(member.size) +
					", C++ " + std::to_string(field.m_offset) + "/" + std::to_string(field.m_size));
			if (member.numeric.matrix.column_count > 0)
				Require(member.numeric.matrix.stride == 16 &&
					(member.decoration_flags & SPV_REFLECT_DECORATION_COLUMN_MAJOR) != 0,
					"compiled matrices must use the C++ column-major sixteen-byte stride");
		}
	}

	ShaderSetPtr Load(std::string_view path, const TVector<std::string>& defines)
	{
		const auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(path);
		ShaderSetPtr shader;
		Require(info && App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(info->GetFileId(), shader, defines) && shader->IsReady(),
			"production shader variant must compile");
		return shader;
	}

	class HoldShaderWorkers
	{
	public:
		HoldShaderWorkers() : m_entered(App::GetSubmodule<Tasks::Scheduler>()->GetNumWorkerThreads())
		{
			const auto count = App::GetSubmodule<Tasks::Scheduler>()->GetNumWorkerThreads();
			Require(count > 0, "shader lifecycle tests require the real Worker queue");
			for (uint32_t i = 0; i < count; ++i)
			{
				auto task = Tasks::CreateTask("Hold shader workers"_h, [this]()
				{
					m_entered.count_down();
					m_release.wait();
				});
				m_tasks.Add(task);
				task->Run();
			}
			m_entered.wait();
		}
		~HoldShaderWorkers()
		{
			m_release.count_down();
			for (auto& task : m_tasks) task->Wait();
		}

	private:
		std::latch m_entered, m_release{ 1 };
		TVector<Tasks::ITaskPtr> m_tasks;
	};

	void CheckShaderValue(const ShaderSetPtr& shader, uint32_t expected)
	{
		for (bool named : { false, true })
		{
			const auto compute = named ? shader->GetDebugComputeShaderRHI() : shader->GetComputeShaderRHI();
			Require(bool(compute), "a loaded shader must publish both compute variants");
			auto task = Tasks::CreateTask<std::string>("Check loaded shader output"_h, [compute, expected]() -> std::string
			{
				try
				{
					auto& driver = Renderer::GetDriver();
					auto commands = Renderer::GetDriverCommands();
					auto buffer = driver->CreateBuffer(2 * sizeof(uint32_t), EBufferUsageBit::StorageBuffer_Bit,
						EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
					Require(buffer && buffer->GetPointer(), "shader output must be host readable");
					auto* values = static_cast<uint32_t*>(buffer->GetPointer());
					values[0] = values[1] = 0xdeadbeefu;
					auto bindings = driver->CreateShaderBindings();
					Require(bool(driver->AddBufferToShaderBindings(bindings, buffer, "outputData"_h, 0)),
						"the loaded shader output buffer must bind");
					auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(command, true);
					commands->Dispatch(command, compute, 1, 1, 1, { bindings });
					commands->MemoryBarrier(command, static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit),
						static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
					commands->EndCommandList(command);
					Require(driver->SubmitCommandList_Immediate(command), "the loaded shader dispatch must complete");
					Require(values[0] == expected && values[1] == 0xdeadbeefu,
						"the loaded shader must write its permutation value and preserve the trailing sentinel");
					return {};
				}
				catch (const std::exception& error) { return error.what(); }
			}, EThreadType::Render);
			task->Run();
			task->Wait();
			Require(task->GetResult().empty(), task->GetResult());
		}
	}

	void RequireShaderFailureDiagnostic()
	{
		std::array<char*, 64> messages{};
		bool found = false;
		while (const auto count = App::PullEditorMessages(messages.data(), static_cast<uint32_t>(messages.size())))
		{
			for (uint32_t i = 0; i < count; ++i)
			{
				const std::string_view text(messages[i]);
				found |= text.find("ShaderLoadRetry.shader") != std::string_view::npos &&
					text.find("expected_retry_failure") != std::string_view::npos;
				delete[] messages[i];
			}
		}
		Require(found, "a failed asynchronous load must publish the shader filename and compiler diagnostic");
	}

	void TestShaderLoadRetry(const std::filesystem::path& workspace)
	{
		auto* compiler = App::GetSubmodule<ShaderCompiler>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		auto* registry = App::GetSubmodule<AssetRegistry>();
		const auto path = workspace / "Content" / "ShaderLoadRetry.shader";
		const auto write = [&](bool repaired)
		{
			YAML::Node document;
			document["defines"].push_back("RECOVER");
			document["glslCommon"] = "#version 450\n";
			std::string source = repaired ? "" : "#ifdef RECOVER\n#error expected_retry_failure\n#endif\n";
			source += R"glsl(
layout(local_size_x = 1) in;
layout(set = 0, binding = 0, std430) buffer OutputData { uint values[]; } outputData;
void main() {
#ifdef RECOVER
	outputData.values[0] = 29u;
#else
	outputData.values[0] = 17u;
#endif
}
)glsl";
			document["glslCompute"] = source;
			std::ofstream output(path);
			output << document;
			output.close();
			Require(static_cast<bool>(output), "shader retry fixture must be written");
		};
		write(false);
		const auto id = registry->GetOrLoadFile(Workspace::PathToUtf8(path));
		scheduler->WaitIdle({ EThreadType::Worker, EThreadType::Render, EThreadType::RHI, EThreadType::Main });
		ShaderSetPtr healthy;
		auto healthyTask = compiler->LoadShader(id, healthy);
		Require(bool(healthyTask), "the healthy permutation must return a real load task");
		healthyTask->Wait();
		Require(healthyTask->GetResult() == healthy && healthy && healthy->IsReady(), "the healthy permutation must load");
		const auto healthyStage = healthy->GetComputeShaderRHI();
		CheckShaderValue(healthy, 17);

		ShaderSetPtr failed;
		Tasks::TaskPtr<ShaderSetPtr> first;
		{
			HoldShaderWorkers hold;
			first = compiler->LoadShader(id, failed, { "RECOVER" });
			Require(first && !first->IsFinished() && failed && !failed->IsReady(), "the cold load must remain pending on Worker");
			compiler->CollectGarbage();
			ShaderSetPtr duplicate;
			Require(compiler->LoadShader(id, duplicate, { "RECOVER" }) == first && duplicate == failed,
				"garbage collection must retain and share the pending load, not create a completed placeholder");
		}
		first->Wait();
		Require(!first->GetResult() && !failed, "a failed compile must complete without publishing a usable shader");
		RequireShaderFailureDiagnostic();
		auto second = compiler->LoadShader(id, failed, { "RECOVER" });
		Require(second && second != first, "failure must evict its promise so the same permutation can be attempted again");
		second->Wait();
		Require(!second->GetResult() && !failed, "an unchanged invalid shader must fail its new attempt cleanly");
		RequireShaderFailureDiagnostic();
		compiler->CollectGarbage();
		ShaderSetPtr stillHealthy;
		auto collected = compiler->LoadShader(id, stillHealthy);
		Require(collected && collected != healthyTask && collected->IsFinished() && collected->GetResult() == healthy &&
			stillHealthy == healthy && healthy->GetComputeShaderRHI() == healthyStage,
			"finished promise collection and failed-permutation eviction must preserve the ready healthy shader");
		CheckShaderValue(stillHealthy, 17);

		write(true);
		Require(App::UpdateAsset(id.ToString().c_str()), "repairing the same shader asset must complete its normal update");
		const auto repairedInfo = registry->GetAssetInfoPtr("ShaderLoadRetry.shader");
		Require(repairedInfo && repairedInfo->GetFileId() == id, "repair must preserve the registered asset identity");
		ShaderSetPtr repaired;
		auto retry = compiler->LoadShader(id, repaired, { "RECOVER" });
		Require(retry && retry != second, "the repaired permutation must get a fresh load task");
		retry->Wait();
		Require(retry->GetResult() == repaired && repaired && repaired->IsReady(), "the repaired load must publish a complete shader");
		CheckShaderValue(repaired, 29);
		CheckShaderValue(healthy, 17);
		compiler->CollectGarbage();
		ShaderSetPtr reused;
		auto warm = compiler->LoadShader(id, reused, { "RECOVER" });
		Require(warm && warm != retry && warm->IsFinished() && warm->GetResult() == repaired && reused == repaired,
			"a repaired shader must survive promise collection and support warm reuse");
		scheduler->WaitIdle({ EThreadType::Worker, EThreadType::Render, EThreadType::RHI, EThreadType::Main });
		std::cout << "Shader load lifecycle: pending/finished GC, shared request, two diagnosed failures, same-asset repair, healthy permutation and debug/optimized GPU values passed\n";
	}

	template<typename Instance>
	void CheckCullingReadback(RHIShaderPtr shader)
	{
		constexpr uint32_t Count = Renderer::GPUCullingGroupSize + 3;
		constexpr uint32_t FirstInstance = 7;
		constexpr uint32_t FirstCandidate = 5;
		constexpr uint32_t FirstOutput = FirstCandidate + Count + 11;
		constexpr uint32_t Sentinel = 0xdeadbeefu;
		const auto bIsVisible = [](uint32_t id) { return id % 5 < 3; };

		// Keep the wrong 192-byte stride within initialized storage, so the RED
		// case proves incorrect object selection rather than an out-of-bounds read.
		std::vector<Instance> instances(2 * (FirstInstance + Count));
		for (uint32_t id = 0; id < instances.size(); ++id)
		{
			instances[id].model = glm::translate(glm::mat4(1), glm::vec3(bIsVisible(id) ? 0 : 1000, 0, -10));
			instances[id].sphereBounds = glm::vec4(0, 0, 0, 0.5f);
		}
		std::vector<uint32_t> indices(FirstOutput + Count + 3, Sentinel);
		for (uint32_t i = 0; i < Count; ++i) indices[FirstCandidate + i] = FirstInstance + i;
		std::array<DrawIndexedIndirectData, 2> batches{};
		for (uint32_t i = 0; i < batches.size(); ++i)
		{
			batches[i].m_indexCount = 3 + 3 * i;
			batches[i].m_instanceCount = i == 0 ? Count / 2 : Count - Count / 2;
			batches[i].m_firstIndex = 3 * i;
			batches[i].m_vertexOffset = static_cast<int32_t>(i) - 1;
			batches[i].m_firstInstance = FirstOutput + (i == 0 ? 0 : Count / 2);
		}
		UboFrameData frame{};
		frame.m_view = glm::mat4(1);
		frame.m_projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
		frame.m_invProjection = glm::inverse(frame.m_projection);
		frame.m_viewportSize = glm::ivec2(64);
		frame.m_cameraZNearZFar = glm::vec2(0.1f, 100.0f);

		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const auto hostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
		const auto buffer = [&](const void* data, size_t size, bool bUniform = false)
		{
			auto result = driver->CreateBuffer(size, bUniform ? EBufferUsageBit::UniformBuffer_Bit : EBufferUsageBit::StorageBuffer_Bit, hostMemory);
			Require(result && result->GetPointer(), "culling fixture buffers must be host accessible");
			std::memcpy(result->GetPointer(), data, size);
			return result;
		};
		auto storage = buffer(instances.data(), instances.size() * sizeof(Instance));
		auto indexBuffer = buffer(indices.data(), indices.size() * sizeof(uint32_t));
		auto indirectBuffer = buffer(batches.data(), sizeof(batches));
		auto frameBuffer = buffer(&frame, sizeof(frame), true);
		TVector<RHIShaderBindingSetPtr> bindings;
		for (uint32_t i = 0; i < 4; ++i) bindings.Add(driver->CreateShaderBindings());
		driver->AddBufferToShaderBindings(bindings[1], storage, "data"_h, 0);
		driver->AddBufferToShaderBindings(bindings[1], indexBuffer, "instanceIndices"_h, 1);
		driver->AddBufferToShaderBindings(bindings[2], indirectBuffer, "drawIndexedIndirect"_h, 0);
		driver->AddBufferToShaderBindings(bindings[3], frameBuffer, "frame"_h, 0);

		auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(command, true);
		GpuCullingPushConstants constants{};
		constants.m_numBatches = static_cast<uint32_t>(batches.size());
		constants.m_numInstances = Count;
		constants.m_firstInstanceIndex = FirstOutput;
		constants.m_firstCandidateInstance = FirstCandidate;
		RecordGpuCullingDispatches(*commands, command, shader, bindings, constants, Renderer::GPUCullingGroupSize, false);
		commands->MemoryBarrier(command, static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
		commands->EndCommandList(command);
		Require(driver->SubmitCommandList_Immediate(command), "culling fixture must complete its GPU dispatches");

		const auto* actualIndices = static_cast<const uint32_t*>(indexBuffer->GetPointer());
		const auto* actualBatches = static_cast<const DrawIndexedIndirectData*>(indirectBuffer->GetPointer());
		for (uint32_t batch = 0; batch < batches.size(); ++batch)
		{
			const auto& expected = batches[batch];
			uint32_t visible = 0;
			for (uint32_t i = 0; i < expected.m_instanceCount; ++i)
			{
				const uint32_t id = FirstInstance + expected.m_firstInstance - FirstOutput + i;
				if (bIsVisible(id))
					Require(actualIndices[expected.m_firstInstance + visible++] == id,
						"GPU culling must retain the exact visible object IDs in order");
			}
			const auto& actual = actualBatches[batch];
			Require(actual.m_instanceCount == visible && actual.m_indexCount == expected.m_indexCount &&
				actual.m_firstIndex == expected.m_firstIndex && actual.m_vertexOffset == expected.m_vertexOffset &&
				actual.m_firstInstance == expected.m_firstInstance,
				"GPU culling must update only the visible count of each indirect draw");
		}
		for (uint32_t i = 0; i < indices.size(); ++i)
			if (i < FirstOutput || i >= FirstOutput + Count)
				Require(actualIndices[i] == indices[i], "GPU culling must preserve candidate indices and output guards");
	}

	void TestCullingReadback()
	{
		for (bool bDepth : { false, true })
		{
			const auto shader = Load("Shaders/ComputeMeshCulling.shader", bDepth ? TVector<std::string>{ "DEPTH_INSTANCE_LAYOUT" } : TVector<std::string>{});
			for (bool bNamed : { true, false })
			{
				const auto compute = bNamed ? shader->GetDebugComputeShaderRHI() : shader->GetComputeShaderRHI();
				auto task = Tasks::CreateTask<std::string>("Culling layout readback"_h, [compute, bDepth]() -> std::string
				{
					try
					{
						if (bDepth) CheckCullingReadback<DepthInstance>(compute);
						else CheckCullingReadback<MainInstance>(compute);
						return {};
					}
					catch (const std::exception& error) { return error.what(); }
				}, EThreadType::Render);
				task->Run();
				task->Wait();
				const auto label = bDepth ? "depth" : "main";
				if (!task->GetResult().empty()) throw std::runtime_error(std::string(label) + " culling readback: " + task->GetResult());
				std::cout << "GPU culling ABI: " << label << (bNamed ? " named" : " optimized") <<
					", 259 candidates, two workgroups, two indirect draws and preserved input/guards passed\n";
			}
		}
	}

	template<typename Verify>
	void Variant(std::string_view path, const TVector<std::string>& defines, Verify&& verify)
	{
		std::string label(path);
		for (const auto& define : defines) label += " " + define;
		bool named = true;
		try
		{
			const auto shader = Load(path, defines);
			// Debug SPIR-V retains names used by material binding. Also verify the
			// optimized modules actually submitted by Release, whose names are stripped.
			for (bool names : { true, false })
			{
				named = names;
				verify(*shader, names);
			}
		}
		catch (const std::exception& error)
		{
			throw std::runtime_error(label + (named ? " [named]: " : " [optimized]: ") + error.what());
		}
		std::cout << "Compiled shader ABI: " << label << " debug/optimized passed\n";
	}
}

namespace Sailor::Tests
{
	void RunShaderLifecycleCommandTests(const std::filesystem::path& workspace)
	{
		TestShaderLoadRetry(workspace);
	}

	void RunShaderInterfaceCommandTests()
	{
		TestCullingReadback();
		for (const TVector<std::string>& defines : { TVector<std::string>{}, { "ALPHA_CUTOUT" },
			{ "SKINNING", "MOTIONS" }, { "ALPHA_CUTOUT", "SKINNING", "MOTIONS", "TRANSMISSION", "MATERIAL_IOR", "CLEAR_COAT", "SHEEN" } })
		{
			Variant("Shaders/Standard_glTF.shader", defines, [&](const ShaderSet& shader, bool names)
			{
				SpirvReflection vertex(names ? shader.GetDebugVertexShaderRHI() : shader.GetVertexShaderRHI());
				CheckFields(vertex.Block(0, 0, false, sizeof(UboFrameData)), FrameFields, names);
				const auto& instance = vertex.Block(2, 0, true, sizeof(MainInstance));
				CheckFields(instance, MainFields, names);
				CheckFields(Member(instance, MainFields.back(), names), MotionFields, names);
				if (defines.Contains("MOTIONS"))
					CheckFields(vertex.Block(0, 1, false, sizeof(UboFrameData)), FrameFields, names);
				if (defines.Contains("SKINNING"))
				{
					CheckFields(vertex.Block(5, 0, true, sizeof(glm::mat4)), BoneFields, names);
					if (defines.Contains("MOTIONS"))
					{
						const auto& previousBones = vertex.Block(0, 2, true, sizeof(glm::mat4));
						const auto& numeric = previousBones.type_description->traits.numeric;
						// Runtime matrix arrays expose their element stride through the type,
						// but not numeric.matrix.stride. Verify shape and column-major packing.
						Require(numeric.matrix.column_count == 4 && numeric.matrix.row_count == 4 && numeric.scalar.width == 32 &&
							(previousBones.decoration_flags & SPV_REFLECT_DECORATION_COLUMN_MAJOR) != 0,
							"previous bone matrices must be column-major float32 mat4 records");
					}
				}
				const auto fragment = names ? shader.GetDebugFragmentShaderRHI() : shader.GetFragmentShaderRHI();
				Require(fragment->m_vulkan.m_shader->GetFragmentOutputMask() == (defines.Contains("MOTIONS") ? 3u : 1u),
					"compiled color/motion outputs must match the requested variant");
			});
		}
		for (bool skinning : { false, true })
		for (bool masked : { false, true })
		{
			TVector<std::string> defines;
			if (skinning) defines.Add("SKINNING");
			if (masked) defines.Add("MASKED");
			Variant("Shaders/DepthOnly.shader", defines, [&](const ShaderSet& shader, bool names)
			{
				SpirvReflection vertex(names ? shader.GetDebugVertexShaderRHI() : shader.GetVertexShaderRHI());
				CheckFields(vertex.Block(0, 0, false, sizeof(UboFrameData)), FrameFields, names);
				CheckFields(vertex.Block(1, 0, true, sizeof(DepthInstance)), DepthFields, names);
				if (skinning) CheckFields(vertex.Block(masked ? 3 : 2, 0, true, sizeof(glm::mat4)), BoneFields, names);
			});
			for (bool evsm : { false, true })
			{
				auto shadowDefines = defines;
				if (evsm) shadowDefines.Add("EVSM");
				Variant("Shaders/ShadowCaster.shader", shadowDefines, [&](const ShaderSet& shader, bool names)
				{
					SpirvReflection vertex(names ? shader.GetDebugVertexShaderRHI() : shader.GetVertexShaderRHI());
					CheckFields(vertex.Block(1, 0, true, sizeof(ShadowInstance)), ShadowFields, names);
					if (skinning) CheckFields(vertex.Block(masked ? 3 : 2, 0, true, sizeof(glm::mat4)), BoneFields, names);
				});
			}
		}
		for (const std::string define : { "FILL", "SUN", "CLOUDS" })
			Variant("Shaders/Sky.shader", { define }, [&](const ShaderSet& shader, bool names)
			{
				SpirvReflection fragment(names ? shader.GetDebugFragmentShaderRHI() : shader.GetFragmentShaderRHI());
				CheckFields(fragment.Block(1, 0, false, sizeof(SkyParameters)), SkyFields, names);
			});
		Variant("Shaders/ComputeLightCulling.shader", {}, [&](const ShaderSet& shader, bool names)
		{
			SpirvReflection compute(names ? shader.GetDebugComputeShaderRHI() : shader.GetComputeShaderRHI());
			CheckFields(compute.Block(0, 0, true, sizeof(RHILightShaderData)), LightFields, names);
			CheckFields(compute.Block(2, 0, false, sizeof(UboFrameData)), FrameFields, names);
		});
		for (bool depth : { false, true })
		for (bool occlusion : { false, true })
		{
			TVector<std::string> defines;
			if (depth) defines.Add("DEPTH_INSTANCE_LAYOUT");
			if (occlusion) defines.Add("OCCLUSION_CULLING");
			Variant("Shaders/ComputeMeshCulling.shader", defines, [&](const ShaderSet& shader, bool names)
			{
				SpirvReflection compute(names ? shader.GetDebugComputeShaderRHI() : shader.GetComputeShaderRHI());
				CheckFields(compute.Block(3, 0, false, sizeof(UboFrameData)), FrameFields, names);
				CheckFields(compute.Block(2, 0, true, sizeof(DrawIndexedIndirectData)), IndirectFields, names);
				if (depth) CheckFields(compute.Block(1, 0, true, sizeof(DepthInstance)), DepthFields, names);
				else
				{
					std::array<Field, 9> fields;
					std::copy_n(MainFields.begin(), 7, fields.begin());
					fields[7] = { "previousModel"_h, offsetof(MainInstance, motion) + offsetof(RHIObjectMotionData, m_previousModel), sizeof(glm::mat4) };
					fields[8] = { "motionState"_h, offsetof(MainInstance, motion) + offsetof(RHIObjectMotionData, m_state), sizeof(glm::uvec4) };
					CheckFields(compute.Block(1, 0, true, sizeof(MainInstance)), fields, names);
				}
			});
		}
		Variant("Shaders/ComputeBloomDownscale.shader", {}, [&](const ShaderSet& shader, bool names)
		{
			using Payload = BloomLayout::PushConstantsDownscale;
			const std::array fields{
				Field{ "u_threshold"_h, offsetof(Payload, m_threshold), sizeof(Payload::m_threshold) },
				Field{ "u_use_threshold"_h, offsetof(Payload, m_bIsThresholdEnabled), sizeof(Payload::m_bIsThresholdEnabled) }
			};
			SpirvReflection compute(names ? shader.GetDebugComputeShaderRHI() : shader.GetComputeShaderRHI());
			CheckFields(compute.PushConstants(sizeof(Payload)), fields, names);
		});
		Variant("Shaders/ComputeBloomUpscale.shader", {}, [&](const ShaderSet& shader, bool names)
		{
			using Payload = BloomLayout::PushConstantsUpscale;
			const std::array fields{
				Field{ "u_mip_level"_h, offsetof(Payload, m_mipLevel), sizeof(Payload::m_mipLevel) },
				Field{ "u_bloom_intensity"_h, offsetof(Payload, m_bloomIntensity), sizeof(Payload::m_bloomIntensity) },
				Field{ "u_dirt_intensity"_h, offsetof(Payload, m_dirtIntensity), sizeof(Payload::m_dirtIntensity) },
				Field{ "u_scatter"_h, offsetof(Payload, m_scatter), sizeof(Payload::m_scatter) }
			};
			SpirvReflection compute(names ? shader.GetDebugComputeShaderRHI() : shader.GetComputeShaderRHI());
			CheckFields(compute.PushConstants(sizeof(Payload)), fields, names);
		});
		std::cout << "Compiled shader ABI: 26 production variants, named and optimized SPIR-V, offsets, sizes, array/matrix strides, nested motion, push constants and MRT outputs passed\n";
	}
}
