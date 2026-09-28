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

	FileId WriteShader(const std::filesystem::path& workspace, const char* name, bool valid = true, bool compute = false)
	{
		const auto path = workspace / "Content" / (std::string(name) + ".shader");
		YAML::Node shader;
		shader["glslCommon"] = "#version 450\n";
		if (compute)
		{
			shader["glslCompute"] = "layout(local_size_x = 1) in; void main() {}";
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
		run("Layout and cold parity", [&]() { TestLayoutAndColdParity(workspace); });
		run("Live shader edit", [&]() { TestLiveShaderEdit(workspace); });
		run("Private instance", [&]() { TestPrivateInstance(workspace); });
		if (!failures.empty()) throw std::runtime_error(failures);
		std::cout << "Material importer preparation, ordered publication, dependency retry and GPU parity tests passed\n";
	}
}
