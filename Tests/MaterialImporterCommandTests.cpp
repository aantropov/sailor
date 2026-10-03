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
#include "Raytracing/PathTracer.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "RHI/Material.h"
#include "RHI/Renderer.h"
#include "RHI/Cubemap.h"
#include "RHI/Lighting.h"
#include "RHI/GlobalIllumination.h"
#include "RHI/Mesh.h"
#include "RHI/RenderTarget.h"
#include "RHI/Surface.h"
#include "FrameGraph/RenderSceneNode.h"
#include "Support/SurfaceRender.h"
#include <glm/gtc/packing.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <latch>
#include <thread>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace Sailor;
using Sailor::Tests::SurfacePixels;
using Sailor::Tests::RenderSurface;

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

	void TestMaterialCreationDefaults(const std::filesystem::path& workspace)
	{
		auto* importer = App::GetSubmodule<MaterialImporter>();
		MaterialAsset::Data data;
		data.m_shader = FileId("1A4BA353-FDA4-4F65-941F-D9FFEE4630A0");
		const auto path = workspace / "Content" / "NeutralMaterial.mat";
		const FileId id = importer->CreateMaterialAsset(path.string(), data);
		const auto document = YAML::LoadFile(path.string());
		Require(document["uniformsVec4"]["material.baseColorFactor"] &&
			document["uniformsVec4"]["material.baseColorFactor"].as<glm::vec4>() == glm::vec4(1),
			"material creation must persist the shader's neutral base color before any runtime load");
		Require(document["uniformsVec4"]["material.emissiveFactor"].as<glm::vec4>() == glm::vec4(0),
			"a new surface must not emit light by default");
		const TMap<std::string, float> expected{
			{ "material.roughnessFactor", 1.0f }, { "material.metallicFactor", 0.0f },
			{ "material.normalScale", 1.0f }, { "material.alphaCutoff", 0.5f },
			{ "material.occlusionStrength", 1.0f }
		};
		for (const auto& entry : expected)
		{
			Require(document["uniformsFloat"][entry.m_first].as<float>() == *entry.m_second,
				"material creation must persist all neutral surface factors");
		}
		Require(!document["samplers"] || document["samplers"].size() == 0,
			"a textureless material must keep the sampler-zero fallback");
		MaterialPtr neutral;
		Require(importer->LoadMaterial_Immediate(id, neutral) && neutral, "neutral material must load");
		Drain();
		const auto neutralBytes = ReadGpu(neutral->GetShaderBindings());
		CheckGpuColor(neutral->GetShaderBindings(), neutralBytes, glm::vec4(1));
		MaterialAsset::Data reference = data;
		reference.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(1);
		reference.m_uniformsVec4["material.emissiveFactor"] = glm::vec4(0);
		reference.m_uniformsFloat = expected;
		const auto referencePath = workspace / "Content" / "ExplicitNeutral.mat";
		std::ofstream referenceOutput(referencePath);
		referenceOutput << MaterialAsset::Serialize(reference);
		referenceOutput.close();
		Require(static_cast<bool>(referenceOutput), "explicit reference material must be written");
		const FileId referenceId = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(referencePath.string());
		MaterialPtr referenceMaterial;
		Require(importer->LoadMaterial_Immediate(referenceId, referenceMaterial) && referenceMaterial,
			"an explicitly authored neutral material must load");
		Drain();
		Require(ReadGpu(referenceMaterial->GetShaderBindings()) == neutralBytes,
			"shader defaults must match the complete GPU payload of an explicitly authored neutral material");
		importer->OnUpdateAssetInfo(App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(id), true);
		Drain();
		Require(ReadGpu(neutral->GetShaderBindings()) == neutralBytes,
			"reloading a canonical material must preserve its complete GPU payload");

		data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(0.2f, 0.4f, 0.8f, 0.35f);
		data.m_uniformsVec4["material.emissiveFactor"] = glm::vec4(2, 3, 4, 0);
		data.m_uniformsFloat["material.roughnessFactor"] = 0.0f;
		data.m_uniformsFloat["material.metallicFactor"] = 0.8f;
		data.m_uniformsFloat["material.alphaCutoff"] = 0.25f;
		data.m_shaderDefines.Add("ALPHA_CUTOUT");
		data.m_renderQueue = "Masked";
		data.m_samplers["baseColorSampler"] = WriteTexture(workspace, "AuthoredBaseColor", true);
		const auto authoredPath = workspace / "Content" / "AuthoredMaterial.mat";
		const FileId authoredId = importer->CreateMaterialAsset(authoredPath.string(), data);
		const auto authored = importer->LoadMaterialAsset(authoredId);
		for (const auto& entry : data.m_uniformsVec4)
		{
			const glm::vec4* value = nullptr;
			Require(authored->GetUniformsVec4().Find(entry.m_first, value) && *value == *entry.m_second,
				"shader defaults must preserve authored colors and emission");
		}
		for (const auto& entry : data.m_uniformsFloat)
		{
			const float* value = nullptr;
			Require(authored->GetUniformsFloat().Find(entry.m_first, value) && *value == *entry.m_second,
				"shader defaults must preserve authored factors, including zero");
		}
		const FileId* sampler = nullptr;
		Require(authored->GetSamplers().Find("baseColorSampler", sampler) &&
			*sampler == data.m_samplers["baseColorSampler"] && authored->GetRenderQueue() == "Masked" &&
			authored->GetShaderDefines().Contains("ALPHA_CUTOUT"),
			"material creation must preserve the canonical texture slot and masked coverage");

		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto shader = YAML::LoadFile(registry->GetAssetInfoPtr(data.m_shader)->GetAssetFilepath());
		const glm::vec4 customColor(0.25f, 0.5f, 0.75f, 1);
		shader["defaultUniformsVec4"]["material.baseColorFactor"] = customColor;
		shader["defaultUniformsFloat"]["material.roughnessFactor"] = 0.5f;
		const auto shaderPath = workspace / "Content" / "CustomNeutral.shader";
		std::ofstream shaderOutput(shaderPath);
		shaderOutput << shader;
		shaderOutput.close();
		Require(static_cast<bool>(shaderOutput), "custom shader description must be written");
		MaterialAsset::Data custom;
		custom.m_shader = registry->GetOrLoadFile(shaderPath.string());
		custom.m_uniformsFloat["material.roughnessFactor"] = 0.0f;
		const auto customPath = workspace / "Content" / "CustomNeutral.mat";
		const FileId customId = importer->CreateMaterialAsset(customPath.string(), custom);
		const auto customDocument = YAML::LoadFile(customPath.string());
		Require(customDocument["uniformsVec4"]["material.baseColorFactor"].as<glm::vec4>() == customColor &&
			customDocument["uniformsFloat"]["material.roughnessFactor"].as<float>() == 0.0f,
			"shader-described defaults must work for arbitrary shader IDs without replacing authored values");
		MaterialPtr customMaterial;
		Require(importer->LoadMaterial_Immediate(customId, customMaterial) && customMaterial,
			"a material created from a custom shader description must load");
		Drain();
		CheckGpuColor(customMaterial->GetShaderBindings(), ReadGpu(customMaterial->GetShaderBindings()), customColor);
		std::cout << "Shader-described material defaults: persisted values, authored overrides and GPU upload passed\n";
	}

	class SurfaceRenderNode final : public Framegraph::RenderSceneNode
	{
	public:
		TRefPtr<SubmissionResources> GetResources(const RHI::RHISceneViewSnapshot& scene)
		{
			return scene.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(this, 0u, 0u);
		}
	};
}

namespace Sailor::Tests
{
	SurfacePixels RenderSurface(MaterialPtr source, RHI::RHIMeshPtr inputMesh)
	{
		SurfacePixels pixels{};
		auto task = Tasks::CreateTaskWithResult<std::string>("Render Standard glTF material reference", [&]() -> std::string
			{
				try
				{
					using namespace RHI;
					constexpr uint32_t side = 8;
					const auto hostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
					auto& driver = Renderer::GetDriver();
					auto commands = Renderer::GetDriverCommands();
					auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(upload, true);
					commands->BeginCommandList(draw, true);
					auto color = driver->CreateRenderTarget(upload, glm::ivec2(side), 1, EFormat::R16G16B16A16_SFLOAT);
					auto depth = driver->CreateRenderTarget(upload, glm::ivec2(side), 1, EFormat::D32_SFLOAT_S8_UINT,
						ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit);
					auto environment = driver->CreateCubemap(glm::ivec2(1), 1, EFormat::R16G16B16A16_SFLOAT);
					commands->ImageMemoryBarrier(upload, environment, EImageLayout::TransferDstOptimal);
					commands->ClearImage(upload, environment, glm::vec4(0.15f, 0.15f, 0.15f, 1));
					commands->ImageMemoryBarrier(upload, environment, EImageLayout::ShaderReadOnlyOptimal);
					auto texture = driver->CreateRenderTarget(upload, glm::ivec2(1), 1, EFormat::R32G32B32A32_SFLOAT);
					commands->ImageMemoryBarrier(upload, texture, EImageLayout::TransferDstOptimal);
					commands->ClearImage(upload, texture, glm::vec4(1));
					commands->ImageMemoryBarrier(upload, texture, EImageLayout::ShaderReadOnlyOptimal);
					RHISceneViewSnapshot scene;
					scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
					scene.m_submissionContext->BeginSubmission(1, 0);
					scene.m_frameBindings = driver->CreateShaderBindings();
					UboFrameData frame{};
					frame.m_view = frame.m_projection = frame.m_invProjection = glm::mat4(1);
					frame.m_cameraPosition = glm::vec4(0, 0, 3, 1);
					frame.m_viewportSize = glm::ivec2(side);
					frame.m_cameraZNearZFar = glm::vec2(0.1f, 10);
					auto frameBinding = driver->AddBufferToShaderBindings(scene.m_frameBindings, "frameData",
						sizeof(frame), 0, EShaderBindingType::UniformBuffer);
					commands->UpdateShaderBinding(upload, frameBinding, &frame, sizeof(frame));
					scene.m_rhiLightsData = driver->CreateShaderBindings();
					auto shader = source->GetShader();
					// Reflection needs names; the draw below uses the regular material shaders.
					Require(driver->FillShadersLayout(scene.m_rhiLightsData,
						{ shader->GetDebugVertexShaderRHI(), shader->GetDebugFragmentShaderRHI() }, 1),
						"surface lighting layout must reflect the actual shader");
					const auto layouts = scene.m_rhiLightsData->GetLayoutBindings();
					for (const auto& layout : layouts)
					{
						if (layout.m_type == EShaderBindingType::UniformBuffer || layout.m_type == EShaderBindingType::StorageBuffer)
						{
							TVector<uint8_t> zeros;
							// Match the GI producer: reflection does not size this flat SSBO header.
							const size_t size = layout.m_name == "globalIlluminationHeader" ? sizeof(RHIGlobalIlluminationGpuHeader) :
								(std::max)({ layout.m_size, layout.m_paddedSize, 16u });
							zeros.Resize(size);
							auto binding = layout.m_type == EShaderBindingType::StorageBuffer ?
								driver->AddSsboToShaderBindings(scene.m_rhiLightsData, layout.m_name, zeros.Num(), 1, layout.m_binding, true) :
								driver->AddBufferToShaderBindings(scene.m_rhiLightsData, layout.m_name, zeros.Num(), layout.m_binding, layout.m_type);
							commands->UpdateShaderBinding(upload, binding, zeros.GetData(), zeros.Num());
						}
						else
						{
							Require(layout.m_type == EShaderBindingType::CombinedImageSampler, "unexpected surface lighting descriptor");
							const bool cube = layout.m_binding == 3 || layout.m_binding == 5 || layout.m_binding == 21 || layout.m_binding == 22;
							TVector<RHITexturePtr> samplers;
							samplers.Resize((std::max)(1u, layout.m_arrayCount));
							for (auto& sampler : samplers) sampler = cube ? RHITexturePtr(environment) : texture;
							Require(driver->AddSamplerToShaderBindings(scene.m_rhiLightsData, layout.m_name, samplers, layout.m_binding).IsValid(),
								"surface environment samplers must bind");
						}
					}
					RHILightShaderData sunlight;
					sunlight.m_type = static_cast<uint32_t>(ELightType::Directional);
					sunlight.m_direction = glm::vec3(0, 0, -1);
					sunlight.m_intensity = glm::vec3(2);
					commands->UpdateShaderBinding(upload, scene.m_rhiLightsData->GetOrAddShaderBinding("light"), &sunlight, sizeof(sunlight));
					const glm::uvec2 grid(0, 1);
					commands->UpdateShaderBinding(upload, scene.m_rhiLightsData->GetOrAddShaderBinding("lightsGrid"), &grid, sizeof(grid));

					auto mesh = inputMesh;
					if (!mesh)
					{
						const auto description = driver->GetOrAddVertexDescription<VertexP3N3T3B3UV2C4>();
						std::array<VertexP3N3T3B3UV2C4, 3> vertices{};
						const glm::vec3 positions[] = { {-1, -1, 0.5f}, {3, -1, 0.5f}, {-1, 3, 0.5f} };
						for (uint32_t i = 0; i < vertices.size(); ++i)
						{
							vertices[i].m_position = positions[i];
							vertices[i].m_normal = glm::vec3(0, 0, 1);
							vertices[i].m_tangent = glm::vec3(1, 0, 0);
							vertices[i].m_bitangent = glm::vec3(0, 1, 0);
							vertices[i].m_color = glm::vec4(1);
							vertices[i].m_texcoord = glm::vec2(0.5f);
						}
						const uint32_t indices[] = { 0, 1, 2 };
						mesh = RHIMeshPtr::Make();
						mesh->m_vertexDescription = description;
						mesh->m_vertexBuffer = driver->CreateBuffer(sizeof(vertices), EBufferUsageBit::VertexBuffer_Bit, hostMemory);
						mesh->m_indexBuffer = driver->CreateBuffer(sizeof(indices), EBufferUsageBit::IndexBuffer_Bit, hostMemory);
						std::memcpy(mesh->m_vertexBuffer->GetPointer(), vertices.data(), sizeof(vertices));
						std::memcpy(mesh->m_indexBuffer->GetPointer(), indices, sizeof(indices));
					}
					auto material = source->GetOrAddRHI(mesh->m_vertexDescription);
					Require(material.IsValid(), "surface graphics material must be ready");
					auto graph = RHIFrameGraphPtr::Make();
					graph->SetRenderTarget("DepthBuffer", depth);
					auto node = TRefPtr<SurfaceRenderNode>::Make();
					node->SetString("Tag", "SurfaceReference");
					node->SetString("GPUCulling", "false");
					node->SetRHIResource("color", RHISurfacePtr::Make(color, color, false));
					auto resources = node->GetResources(scene);
					RHIBatch batch(material, mesh);
#if defined(__APPLE__)
					Framegraph::TextureBindingCache textureCache;
					TVector<uint32_t> requested;
					for (const auto& sampler : source->GetSamplers())
						requested.Add(static_cast<uint32_t>(App::GetSubmodule<TextureImporter>()->GetTextureIndex(sampler.m_second->GetFileId())));
					NormalizeTextureSamplers(requested);
					uint32_t supported = 0;
					bool current = false;
					batch.m_textureBindings = Framegraph::Details::GetTextureBindingSet(textureCache, requested, 1, supported, current);
					Require(current, "surface textures must have current remapping");
#else
					batch.m_textureBindings = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
#endif
					SurfaceRenderNode::PerInstanceData instance{};
					instance.model = glm::mat4(1);
					instance.materialInstance = source->GetShaderBindings()->GetStorageInstanceIndex("material");
					resources->m_packet.Add(batch, mesh, instance);
					resources->m_packet.Finalize();
					commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
						static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
					commands->ImageMemoryBarrier(draw, color, EImageLayout::TransferDstOptimal);
					commands->ClearImage(draw, color, glm::vec4(-1));
					node->Process(graph, upload, draw, scene);
					Require(node->GetDrawCallStats().m_numInstances == 1, "surface reference must issue a real draw");
					auto readback = driver->CreateBuffer(side * side * 8, EBufferUsageBit::BufferTransferDst_Bit, hostMemory);
					commands->ImageMemoryBarrier(draw, color, EImageLayout::TransferSrcOptimal);
					commands->CopyImageToBuffer(draw, color, readback);
					auto headerReadback = driver->CreateBuffer(sizeof(RHIGlobalIlluminationGpuHeader), EBufferUsageBit::BufferTransferDst_Bit, hostMemory);
					auto header = scene.m_rhiLightsData->GetOrAddShaderBinding("globalIlluminationHeader");
					draw->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
					draw->m_vulkan.m_commandBuffer->CopyBuffer(*header->m_vulkan.m_valueBinding->Get(),
						*headerReadback->m_vulkan.m_buffer->Get(), sizeof(RHIGlobalIlluminationGpuHeader));
					commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
					commands->EndCommandList(upload);
					commands->EndCommandList(draw);
					auto ready = driver->CreateWaitSemaphore();
					auto uploadFence = RHIFencePtr::Make();
					auto drawFence = RHIFencePtr::Make();
					Require(driver->SubmitCommandList(upload, uploadFence, ready) && driver->SubmitCommandList(draw, drawFence, nullptr, ready),
						"surface upload and draw must submit");
					Require(drawFence->Wait(5000000000ull) == EFenceStatus::Finished && uploadFence->Wait(5000000000ull) == EFenceStatus::Finished,
						"surface reference must finish within bounded fence waits");
					const auto* packed = static_cast<const uint32_t*>(readback->GetPointer());
					const auto* headerWords = static_cast<const uint32_t*>(headerReadback->GetPointer());
					for (uint32_t word = 0; word < sizeof(RHIGlobalIlluminationGpuHeader) / sizeof(uint32_t); ++word)
						Require(headerWords[word] == 0, "surface fixture GI and debug controls must be zero on GPU");
					for (uint32_t i = 0; i < pixels.size(); ++i)
						pixels[i] = glm::vec4(glm::unpackHalf2x16(packed[i * 2]), glm::unpackHalf2x16(packed[i * 2 + 1]));
					uploadFence->ClearDependencies();
					drawFence->ClearDependencies();
					return {};
				}
				catch (const std::exception& error) { return error.what(); }
			}, EThreadType::Render);
		task->Run();
		task->Wait();
		if (!task->GetResult().empty()) throw std::runtime_error(task->GetResult());
		return pixels;
	}
}

namespace
{
	void TestStandardGltfSurfaceRendering(const std::filesystem::path& workspace)
	{
		auto* importer = App::GetSubmodule<MaterialImporter>();
		auto* registry = App::GetSubmodule<AssetRegistry>();
		const auto texture = WriteTexture(workspace, "SurfaceBaseColor", true);
		std::array<SurfacePixels, 8> results;
		const char* names[] = { "neutral", "emissive", "metal", "smooth", "textured", "cutout", "cutoff override", "owned Box" };
		for (uint32_t scenario = 0; scenario < results.size(); ++scenario)
		{
			MaterialAsset::Data data;
			data.m_shader = FileId("1A4BA353-FDA4-4F65-941F-D9FFEE4630A0");
			data.m_renderQueue = "SurfaceReference";
			data.m_renderState = RHI::RenderState(false, false, 0, false, RHI::ECullMode::None,
				RHI::EBlendMode::None, RHI::EFillMode::Fill, 0, false);
			if (scenario == 1) data.m_uniformsVec4["material.emissiveFactor"] = glm::vec4(2, 0.25f, 0.5f, 0);
			if (scenario == 2 || scenario == 3) data.m_uniformsFloat["material.roughnessFactor"] = 0.2f;
			if (scenario == 2) data.m_uniformsFloat["material.metallicFactor"] = 1;
			if (scenario == 4) data.m_samplers["baseColorSampler"] = texture;
			if (scenario == 5 || scenario == 6)
			{
				data.m_shaderDefines.Add("ALPHA_CUTOUT");
				data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(1, 1, 1, 0.35f);
			}
			if (scenario == 6) data.m_uniformsFloat["material.alphaCutoff"] = 0.25f;
			if (scenario == 7) data.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(1, 0.5f, 0.5f, 1);
			MaterialAsset::Data reference = data;
			reference.m_uniformsVec4 = {
				{ "material.baseColorFactor", glm::vec4(1) }, { "material.emissiveFactor", glm::vec4(0) }
			};
			reference.m_uniformsFloat = {
				{ "material.roughnessFactor", 1.0f }, { "material.metallicFactor", 0.0f },
				{ "material.normalScale", 1.0f }, { "material.alphaCutoff", 0.5f }, { "material.occlusionStrength", 1.0f }
			};
			for (const auto& entry : data.m_uniformsVec4) reference.m_uniformsVec4[entry.m_first] = *entry.m_second;
			for (const auto& entry : data.m_uniformsFloat) reference.m_uniformsFloat[entry.m_first] = *entry.m_second;
			const auto prefix = std::string("Surface") + std::to_string(scenario);
			const auto actualPath = workspace / "Content" / (prefix + ".mat");
			FileId actualId;
			if (scenario == 7)
			{
				const auto info = registry->GetAssetInfoPtr("Models/Box/materials/BoxDefault.mat");
				Require(info != nullptr, "owned Box material must be discoverable");
				auto box = YAML::LoadFile(info->GetAssetFilepath());
				box["renderQueue"] = "SurfaceReference";
				box["bEnableDepthTest"] = box["bEnableZWrite"] = box["bSupportMultisampling"] = false;
				box["cullMode"] = "None";
				std::ofstream output(actualPath);
				output << box;
				output.close();
				Require(static_cast<bool>(output), "isolated Box render fixture must be written");
				actualId = registry->GetOrLoadFile(actualPath.string());
			}
			else actualId = importer->CreateMaterialAsset(actualPath.string(), data);
			const auto referencePath = workspace / "Content" / (prefix + "Reference.mat");
			std::ofstream output(referencePath);
			output << MaterialAsset::Serialize(reference);
			output.close();
			Require(static_cast<bool>(output), "explicit surface reference must be written");
			const FileId referenceId = registry->GetOrLoadFile(referencePath.string());
			MaterialPtr material, referenceMaterial;
			Require(importer->LoadMaterial_Immediate(actualId, material) && material &&
				importer->LoadMaterial_Immediate(referenceId, referenceMaterial) && referenceMaterial,
				"both actual surface materials must load");
			Drain();
			CheckGpuColor(material->GetShaderBindings(), ReadGpu(material->GetShaderBindings()),
				reference.m_uniformsVec4["material.baseColorFactor"]);
			const auto actual = RenderSurface(material);
			const auto expected = RenderSurface(referenceMaterial);
			for (size_t i = 0; i < actual.size(); ++i)
			{
				Require(glm::all(glm::lessThan(glm::abs(actual[i] - expected[i]), glm::vec4(0.002f))),
					"actual Standard glTF pixels must match the explicitly authored reference");
				if (scenario == 5)
					Require(actual[i] == glm::vec4(-1), "default alpha cutoff must leave the clear target untouched");
				else if (!(glm::all(glm::greaterThan(glm::vec3(actual[i]), glm::vec3(0.01f))) && actual[i].a > 0))
					throw std::runtime_error(std::string(names[scenario]) + " pixel " + std::to_string(i) +
						" must be visibly lit; RGBA=" + std::to_string(actual[i].r) + "," + std::to_string(actual[i].g) +
						"," + std::to_string(actual[i].b) + "," + std::to_string(actual[i].a));
			}
			results[scenario] = actual;
			std::cout << "Standard glTF rendered " << names[scenario] << ": "
				<< actual[32].r << ',' << actual[32].g << ',' << actual[32].b << ',' << actual[32].a << '\n';
		}
		float metalDifference = 0, roughnessDifference = 0;
		for (size_t i = 0; i < results[0].size(); ++i)
		{
			Require(glm::length(glm::vec3(results[1][i] - results[0][i]) - glm::vec3(2, 0.25f, 0.5f)) < 0.005f,
				"emission must contribute its authored linear radiance to the rendered surface");
			metalDifference += glm::length(glm::vec3(results[2][i] - results[3][i]));
			roughnessDifference += glm::length(glm::vec3(results[3][i] - results[0][i]));
			Require(results[4][i].r > results[4][i].g + 0.05f,
				"the canonical base-color texture slot must visibly tint the surface");
			Require(std::abs(results[6][i].a - 0.35f) < 0.001f,
				"authored cutoff must retain fragments rejected by the default cutoff");
		}
		Require(metalDifference > 0.1f && roughnessDifference > 0.1f,
			"metalness and roughness must independently change actual rendered pixels");
	}

	class HoldRenderQueue
	{
	public:
		HoldRenderQueue()
		{
			m_task = Tasks::CreateTask("Hold shader publication", [this]()
				{
					m_entered = true;
					m_release.wait();
				}, EThreadType::Render);
			m_task->Run();
		}
		~HoldRenderQueue()
		{
			Release();
			m_task->Wait();
		}
		void Wait()
		{
			const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (!m_entered && std::chrono::steady_clock::now() < until) std::this_thread::yield();
			Require(m_entered, "render publication gate must start");
		}
		void Release()
		{
			if (!m_released) m_release.count_down();
			m_released = true;
		}
	private:
		Tasks::TaskPtr<> m_task;
		std::atomic<bool> m_entered{ false };
		std::latch m_release{ 1 };
		bool m_released = false;
	};

	class ShaderReloadObserver final : public Object
	{
	public:
		explicit ShaderReloadObserver(ShaderSetPtr shader) : m_shader(shader) {}
		Tasks::ITaskPtr OnHotReload() override
		{
			m_threads.Add(App::GetSubmodule<Tasks::Scheduler>()->GetCurrentThreadType());
			m_stages.Add(m_shader->GetComputeShaderRHI());
			m_complete &= m_shader->IsReady() && m_shader->GetComputeShaderRHI() && m_shader->GetDebugComputeShaderRHI();
			return {};
		}
		ShaderSetPtr m_shader;
		TVector<EThreadType> m_threads;
		TVector<RHI::RHIShaderPtr> m_stages;
		bool m_complete = true;
	};

	class AssetUpdateObserver final : public IAssetInfoHandlerListener
	{
	public:
		explicit AssetUpdateObserver(AssetInfo& info) : m_handler(*info.GetHandler()), m_fileId(info.GetFileId())
		{
			m_handler.Subscribe(this);
		}
		~AssetUpdateObserver() { m_handler.Unsubscribe(this); }
		void OnImportAsset(AssetInfoPtr) override {}
		void OnUpdateAssetInfo(AssetInfoPtr info, bool) override
		{
			if (info->GetFileId() != m_fileId) return;
			++m_notifications;
			if (!App::GetSubmodule<Tasks::Scheduler>()->IsMainThread()) m_wrongThread = true;
			if (m_afterNotification) m_afterNotification->Run();
		}
		std::atomic<uint32_t> m_notifications{ 0 };
		std::atomic<bool> m_wrongThread{ false };
		Tasks::ITaskPtr m_afterNotification;
	private:
		IAssetInfoHandler& m_handler;
		FileId m_fileId;
	};

	void TestWorkerFileReload(const std::filesystem::path& workspace)
	{
		MaterialFixture fixture(workspace, "WorkerFileReload", {});
		auto material = fixture.Load();
		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		AssetUpdateObserver observer(*fixture.info);
		const auto oldColor = Color(material);
		const auto oldRevision = material->GetContentRevision();
		const glm::vec4 newColor(0.125f, 0.75f, 0.5f, 1);
		auto document = fixture.Read();
		SetColor(document, newColor);
		fixture.Write(document);
		const auto pendingBefore = scheduler->GetNumTasks(EThreadType::Main);

		HoldRenderQueue hold;
		hold.Wait();
		TVector<Tasks::TaskPtr<bool>> lookups;
		for (uint32_t caller = 0; caller < 4; ++caller)
		{
			auto lookup = Tasks::CreateTask<bool>("Request live asset from Worker", [&]()
				{
					bool sameId = true;
					for (uint32_t i = 0; i < 16; ++i)
					{
						sameId &= registry->GetOrLoadFile(fixture.path.string()) == fixture.id;
					}
					return sameId;
				});
			lookup->Run();
			lookups.Add(lookup);
		}
		bool sameIds = true;
		for (const auto& lookup : lookups)
		{
			lookup->Wait();
			sameIds &= lookup->GetResult();
		}
		const bool deferred = observer.m_notifications == 0;
		const bool coalesced = scheduler->GetNumTasks(EThreadType::Main) == pendingBefore + 1;
		const bool unchanged = Color(material) == oldColor && material->GetContentRevision() == oldRevision;
		hold.Release();
		scheduler->ProcessTasksOnMainThread();
		Drain();

		Require(sameIds && deferred && coalesced && unchanged,
			"Worker LoadFile must return the stable ID and coalesce a Main update without notifying from Worker");
		Require(observer.m_notifications == 1 && !observer.m_wrongThread && Color(material) == newColor &&
			App::GetSubmodule<MaterialImporter>()->GetLoadedMaterial(fixture.id) == material,
			"the queued Main update must publish the changed material once and preserve its identity");
		CheckGpuColor(material->GetShaderBindings(), ReadGpu(material->GetShaderBindings()), newColor);
		std::cout << "Direct LoadFile: 64 Worker requests coalesce, defer notifications and retain material identity passed\n";
	}

	void TestFileReloadDuringMainUpdate(const std::filesystem::path& workspace)
	{
		MaterialFixture trigger(workspace, "MainUpdateTrigger", {});
		MaterialFixture target(workspace, "MainUpdateTarget", {});
		auto first = trigger.Load();
		auto second = target.Load();
		auto* registry = App::GetSubmodule<AssetRegistry>();
		AssetUpdateObserver triggerObserver(*trigger.info), targetObserver(*target.info);
		const auto oldColor = Color(second);
		const glm::vec4 newColor(0.75f, 0.25f, 0.5f, 1);
		for (const auto* fixture : { &trigger, &target })
		{
			auto document = fixture->Read();
			SetColor(document, newColor);
			fixture->Write(document);
		}
		auto lookup = Tasks::CreateTask<bool>("Lookup during Main asset dispatch", [&]()
			{
				return registry->GetOrLoadFile(target.path.string()) == target.id;
			});
		triggerObserver.m_afterNotification = lookup;
		const bool updated = App::UpdateAsset(trigger.id.ToString().c_str());
		const bool deferred = targetObserver.m_notifications == 0 && Color(second) == oldColor;
		App::GetSubmodule<Tasks::Scheduler>()->ProcessTasksOnMainThread();
		Drain();
		Require(updated && lookup->IsFinished() && lookup->GetResult() && deferred,
			"a Worker requested by Main reload must finish without waiting for Main dispatch");
		Require(triggerObserver.m_notifications == 1 && targetObserver.m_notifications == 1 &&
			!triggerObserver.m_wrongThread && !targetObserver.m_wrongThread &&
			Color(first) == newColor && Color(second) == newColor,
			"the second asset must update on Main after the triggering reload completes");
		std::cout << "Direct LoadFile: Worker lookup inside Main reload dispatch completes without deadlock passed\n";
	}

	void TestQueuedFileReloadRetry(const std::filesystem::path& workspace)
	{
		MaterialFixture fixture(workspace, "QueuedFileRetry", {});
		auto material = fixture.Load();
		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		AssetUpdateObserver observer(*fixture.info);
		const auto oldColor = Color(material);
		const auto oldRevision = material->GetContentRevision();
		auto document = fixture.Read();
		auto invalid = YAML::Clone(document);
		invalid["shaderUid"] = FileId::CreateNewFileId();
		fixture.Write(invalid);
		auto reload = [&](EThreadType thread)
		{
			const auto notifications = observer.m_notifications.load();
			auto lookup = Tasks::CreateTask<bool>("Request asset retry", [&]()
				{
					return registry->GetOrLoadFile(fixture.path.string()) == fixture.id;
				}, thread);
			lookup->Run();
			lookup->Wait();
			const bool deferred = observer.m_notifications == notifications;
			scheduler->ProcessTasksOnMainThread();
			Drain();
			Require(lookup->GetResult() && deferred && !observer.m_wrongThread &&
				observer.m_notifications == notifications + 1,
				"each off-Main lookup must defer its single notification to Main");
		};
		reload(EThreadType::Worker);
		Require(material->IsReady() && Color(material) == oldColor &&
			material->GetContentRevision() == oldRevision,
			"a failed queued reload must retain the last good material");
		Require(registry->IsAssetExpired(fixture.info),
			"a failed material publication must not acknowledge the source revision in the asset cache");
		reload(EThreadType::Worker);
		Require(material->GetContentRevision() == oldRevision && registry->IsAssetExpired(fixture.info),
			"the same failed source must remain retryable without touching its file");
		{
			std::ofstream output(fixture.path);
			output << "uniformsVec4: [";
		}
		reload(EThreadType::Worker);
		Require(Color(material) == oldColor && material->GetContentRevision() == oldRevision &&
			registry->IsAssetExpired(fixture.info),
			"a parse failure before publication must also retain the material and leave the source retryable");

		uint32_t attempt = 0;
		for (const auto thread : { EThreadType::Render, EThreadType::RHI, EThreadType::Background })
		{
			const glm::vec4 color(0.25f * ++attempt, 0.5f, 0.75f, 1);
			SetColor(document, color);
			fixture.Write(document);
			reload(thread);
			Require(Color(material) == color && !registry->IsAssetExpired(fixture.info) &&
				App::GetSubmodule<MaterialImporter>()->GetLoadedMaterial(fixture.id) == material,
				"later requests must repair or update the same material and acknowledge only a successful revision");
		}
		const auto revision = material->GetContentRevision();
		const auto queued = scheduler->GetNumTasks(EThreadType::Main);
		HoldRenderQueue hold;
		hold.Wait();
		const bool unchanged = registry->GetOrLoadFile(fixture.path.string()) == fixture.id;
		hold.Release();
		Require(unchanged && scheduler->GetNumTasks(EThreadType::Main) == queued &&
			observer.m_notifications == 6 && material->GetContentRevision() == revision,
			"unchanged lookup must not queue work or wait for the occupied Render queue");
		std::cout << "Direct LoadFile: failed parsing/publication, retry, Render/RHI/Background callers and warm lookup passed\n";
	}

	void TestMaterialCapturePublication(const std::filesystem::path& workspace)
	{
		using Tracer = Raytracing::PathTracer;
		const auto oldTexture = WriteTexture(workspace, "CaptureOldTexture", true);
		const auto newTexture = WriteTexture(workspace, "CaptureNewTexture", true);
		TexturePtr loaded;
		Require(App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(newTexture, loaded),
			"replacement capture texture must load");
		MaterialFixture fixture(workspace, "MaterialCapturePublication", oldTexture);
		auto material = fixture.Load();
		auto document = fixture.Read();
		auto vectors = document["uniformsVec4"].as<TMap<std::string, glm::vec4>>();
		for (const char* name : { "material.emissiveFactor", "material.emissive", "material.emission" })
		{
			vectors[name] = glm::vec4(2, 4, 8, 0);
		}
		document["uniformsVec4"] = vectors;
		auto floats = document["uniformsFloat"].as<TMap<std::string, float>>();
		floats["material.alphaCutoff"] = 0.25f;
		document["uniformsFloat"] = floats;
		auto samplers = document["samplers"].as<TMap<std::string, FileId>>();
		samplers["baseColorSampler"] = newTexture;
		document["samplers"] = samplers;
		fixture.Write(document);
		Drain();

		HoldRenderQueue hold;
		hold.Wait();
		fixture.Reload();
		auto reload = App::GetSubmodule<MaterialImporter>()->GetLoadPromise(fixture.id);
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		const auto queuedBefore = scheduler->GetNumTasks(EThreadType::Render);
		Tracer::MaterialSnapshots snapshots;
		std::atomic<bool> finished{ false };
		std::jthread capture([&]()
			{
				snapshots = Tracer::CaptureMaterials({ material, material, {} });
				finished = true;
			});
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!finished && scheduler->GetNumTasks(EThreadType::Render) == queuedBefore &&
			std::chrono::steady_clock::now() < until) std::this_thread::yield();
		const bool queuedCapture = scheduler->GetNumTasks(EThreadType::Render) > queuedBefore;
		const bool readDuringPublication = finished;
		hold.Release();
		capture.join();
		reload->Wait();
		Drain();
		Require(queuedCapture && !readDuringPublication,
			"material capture must execute on the publication queue while its owner waits");
		Require(snapshots.Num() == 3 && snapshots[0] == snapshots[1] && !snapshots[2] &&
			snapshots[0]->m_parameters.m_emissiveFactor == glm::vec3(2, 4, 8) &&
			snapshots[0]->m_parameters.m_alphaCutoff == 0.25f,
			"capture must observe the completed material values and retain slot identity");
		bool replacedSampler = false;
		for (const auto& sampler : snapshots[0]->m_samplers)
		{
			if (sampler.m_first == "baseColorSampler")
			{
				replacedSampler = sampler.m_second.m_texture && sampler.m_second.m_texture->m_fileId == newTexture;
			}
		}
		Require(replacedSampler, "material capture must retain the sampler from the same publication");
		std::cout << "Material CPU capture: queued publication, coherent parameters/samplers and slot identity passed\n";
	}

	void TestMaterialCaptureOwnerUpdates(const std::filesystem::path& workspace)
	{
		using Tracer = Raytracing::PathTracer;
		MaterialFixture fixture(workspace, "MaterialCaptureOwner", {});
		auto material = fixture.Load();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		Require(scheduler->IsMainThread(), "material updates must use the real game owner");
		Tracer::MaterialSnapshotCache cache;
		auto first = Tracer::CaptureMaterials({ material }, &cache);
		const auto originalEmission = first[0]->m_parameters.m_emissiveFactor;
		const auto originalCutoff = first[0]->m_parameters.m_alphaCutoff;
		HoldRenderQueue hold;
		hold.Wait();
		const auto queuedBefore = scheduler->GetNumTasks(EThreadType::Render);
		Tracer::MaterialSnapshots reused;
		std::atomic<bool> finished{ false };
		std::jthread reuse([&]()
			{
				reused = Tracer::CaptureMaterials({ material, {}, material }, &cache);
				finished = true;
			});
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!finished && std::chrono::steady_clock::now() < until) std::this_thread::yield();
		const bool reusedWithoutWaiting = finished && scheduler->GetNumTasks(EThreadType::Render) == queuedBefore;
		hold.Release();
		reuse.join();
		Require(reusedWithoutWaiting && reused[0] == first[0] && !reused[1] && reused[2] == first[0] && cache.Num() == 1,
			"unchanged material snapshots must return without queue work even while Render is occupied");

		for (uint32_t step = 1; step <= 16; ++step)
		{
			const float value = static_cast<float>(step) / 16.0f;
			for (const char* name : { "material.emissiveFactor", "material.emissive", "material.emission" })
			{
				material->SetUniform(name, glm::vec4(value, value * 2, value * 4, 0));
			}
			material->SetUniform("material.alphaCutoff", value);
			const auto captured = Tracer::CaptureMaterials({ material }, &cache);
			Require(captured[0]->m_parameters.m_emissiveFactor == glm::vec3(value, value * 2, value * 4) &&
				captured[0]->m_parameters.m_alphaCutoff == value &&
				first[0]->m_parameters.m_emissiveFactor == originalEmission &&
				first[0]->m_parameters.m_alphaCutoff == originalCutoff,
				"successive owner updates must publish coherent captures while retained values remain unchanged");
		}
		auto onRender = Tasks::CreateTask<Tracer::MaterialSnapshots>("Capture from Render owner", [material]()
			{
				return Tracer::CaptureMaterials({ material });
			}, EThreadType::Render);
		onRender->Run();
		onRender->Wait();
		Require(onRender->GetResult()[0]->m_parameters.m_emissiveFactor == glm::vec3(1, 2, 4),
			"a Render caller must capture inline without waiting for its own queue");
		Require(Tracer::CaptureMaterials({}, &cache).IsEmpty() && cache.IsEmpty(),
			"an empty owner capture must evict unused material snapshots");
		std::cout << "Material CPU capture: Main updates, immutable cache reuse and inline Render caller passed\n";
	}

	void TestShaderPublicationQueue(const std::filesystem::path& workspace)
	{
		auto* compiler = App::GetSubmodule<ShaderCompiler>();
		auto& cache = ShaderCompilerTestAccess::GetShaderCache(*compiler);
		const auto includePath = workspace / "Content" / "QueuedShader.glsl";
		auto writeInclude = [&](float value)
		{
			std::ofstream output(includePath);
			output << "const float QueuedValue = " << value << ";\n";
			output.close();
			Require(static_cast<bool>(output), "queued shader include must be written");
		};
		writeInclude(0.25f);
		Require(static_cast<bool>(App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(includePath.string())),
			"queued shader include must be registered");
		const auto id = WriteShader(workspace, "QueuedShader", true, true, "QueuedShader.glsl");
		ShaderSetPtr shader;
		bool loaded = false;
		auto cold = Tasks::CreateTask("Load shader from Render", [&]()
			{
				loaded = compiler->LoadShader_Immediate(id, shader);
			}, EThreadType::Render);
		cold->Run();
		cold->Wait();
		Drain();
		Require(loaded && shader && shader->IsReady(),
			"synchronous cold shader loading from Render must not wait for its own queue");
		const auto coldId = WriteShader(workspace, "ColdDuringShaderReload", true, true);
		Drain();
		const auto generation = ShaderCacheTestAccess::GetGeneration(cache, id, 0);
		const auto previous = shader->GetComputeShaderRHI();
		struct ObserveShader
		{
			Memory::ObjectAllocatorPtr allocator = Memory::ObjectAllocatorPtr::Make();
			TObjectPtr<ShaderReloadObserver> observer;
			~ObserveShader()
			{
				App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(
					{ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
				observer->m_shader->RemoveHotReloadDependentObject(observer);
				observer.DestroyObject(allocator);
			}
		} observation;
		observation.observer = TObjectPtr<ShaderReloadObserver>::Make(observation.allocator, shader);
		shader->AddHotReloadDependentObject(observation.observer);
		HoldRenderQueue hold;
		hold.Wait();
		writeInclude(0.75f);
		auto reload = compiler->OnEffectiveContentChanged("QueuedShader.glsl");
		Require(static_cast<bool>(reload), "include reload must return a completion task");
		auto preparation = ShaderCompilerTestAccess::GetLastPreparation(*compiler);
		Require(preparation && preparation->GetThreadType() == EThreadType::Worker,
			"shader preparation must run on Worker, independently of Render publication");
		preparation->Wait();
		const bool prepared = ShaderCacheTestAccess::GetGeneration(cache, id, 0) != generation;
		const bool publishedWhileHeld = reload->IsFinished();
		const bool retained = shader->GetComputeShaderRHI() == previous;
		ShaderSetPtr coldShader;
		auto coldDuringReload = compiler->LoadShader(coldId, coldShader);
		Require(static_cast<bool>(coldDuringReload), "a cold shader request must be admitted during reload");
		coldDuringReload->Wait();
		Require(coldDuringReload->GetResult() == coldShader && coldShader && coldShader->IsReady() &&
			coldShader->GetComputeShaderRHI() && coldShader->GetDebugComputeShaderRHI(),
			"a cold load must not wait behind a pending Render publication");
		writeInclude(0.875f);
		auto second = compiler->OnEffectiveContentChanged("QueuedShader.glsl");
		preparation = ShaderCompilerTestAccess::GetLastPreparation(*compiler);
		preparation->Wait();
		const bool secondDeferred = !second->IsFinished() && shader->GetComputeShaderRHI() == previous;
		hold.Release();
		reload->Wait();
		second->Wait();
		Drain();
		Require(prepared, "shader compilation must progress while the Render queue is occupied");
		Require(!publishedWhileHeld && retained && secondDeferred,
			"shader reloads must retain the last-good stages until Render publishes each result");
		Require(reload->GetResult() && second->GetResult() && shader->IsReady() && shader->GetComputeShaderRHI() != previous,
			"shader reload must publish the prepared stages after Render becomes available");
		auto observer = observation.observer;
		Require(observer->m_complete && observer->m_threads.Num() == 2 &&
			observer->m_threads[0] == EThreadType::Render && observer->m_threads[1] == EThreadType::Render &&
			observer->m_stages[0] != observer->m_stages[1] && observer->m_stages[1] == shader->GetComputeShaderRHI(),
			"dependents must observe complete, ordered shader replacements on Render");
		std::cout << "Shader publication: Render cold load, Worker preparation, ordered Render reload and notifications passed\n";

		const auto cachePath = ShaderCacheTestAccess::GetCachePath(cache);
		Require(cachePath.parent_path() == std::filesystem::weakly_canonical(workspace / "Cache"),
			"cache recovery must only move the fixture workspace manifest");
		Require(cache.SaveCache(), "shader cache must be committed before simulating missing storage");
		std::filesystem::rename(cachePath, workspace / "ShaderCache.before-recovery.yaml");
		const auto beforeRecovery = shader->GetComputeShaderRHI();
		HoldRenderQueue recoveryGate;
		recoveryGate.Wait();
		Require(compiler->RecoverMissingShaderCacheStorage(), "missing shader cache storage must trigger recovery");
		preparation = ShaderCompilerTestAccess::GetLastPreparation(*compiler);
		preparation->Wait();
		const bool recoveryDeferred = shader->GetComputeShaderRHI() == beforeRecovery;
		recoveryGate.Release();
		Drain();
		Require(recoveryDeferred && shader->IsReady() && shader->GetComputeShaderRHI() != beforeRecovery &&
			observer->m_threads.Num() == 3 && observer->m_threads[2] == EThreadType::Render && observer->m_complete,
			"cache recovery must also retain live shaders until complete Render-owned replacement");
		Require(std::filesystem::is_regular_file(cachePath) && !compiler->RecoverMissingShaderCacheStorage(),
			"recovered cache storage must be committed without repeating recovery");
		std::cout << "Shader cache recovery: complete deferred Render replacement and stable storage passed\n";
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
				auto task = ShaderCompilerTestAccess::ReloadShaderResources(*compiler, uid);
				task->Wait();
				Drain();
				return task->GetResult();
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
			ShaderCacheTestAccess::FailNextArtifactCleanup(cache);
			Require(update() && shader->IsReady(), "a broken artifact must recompile to a complete RHI set");
			const auto repairedGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 0);
			Require(repairedGeneration != generation, "a broken debug artifact must rebuild the complete permutation");
			Require(!cache.IsDirty() && cache.NeedsMaintenance() && shader->IsReady(),
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
		run("Material creation defaults", [&]() { TestMaterialCreationDefaults(workspace); });
		run("Standard glTF rendered reference", [&]() { TestStandardGltfSurfaceRendering(workspace); });
		run("Texture failure and retry", [&]() { TestFailedTextureReload(workspace); });
		run("Cold failure and retry", [&]() { TestColdFailureRetry(workspace); });
		run("Reload ordering", [&]() { TestOrderedReload(workspace, false); });
		run("Cold ordering", [&]() { TestOrderedReload(workspace, true); });
		run("Shader failures", [&]() { TestShaderFailures(workspace); });
		run("Shader publication queue", [&]() { TestShaderPublicationQueue(workspace); });
		run("Worker file reload", [&]() { TestWorkerFileReload(workspace); });
		run("File reload during Main update", [&]() { TestFileReloadDuringMainUpdate(workspace); });
		run("Queued file reload retry", [&]() { TestQueuedFileReloadRetry(workspace); });
		run("Material capture publication", [&]() { TestMaterialCapturePublication(workspace); });
		run("Material capture owner updates", [&]() { TestMaterialCaptureOwnerUpdates(workspace); });
		run("Warm shader permutation", [&]() { TestWarmShaderPermutation(workspace); });
		run("Shared shader reload", [&]() { TestSharedShaderReload(workspace); });
		run("Layout and cold parity", [&]() { TestLayoutAndColdParity(workspace); });
		run("Live shader edit", [&]() { TestLiveShaderEdit(workspace); });
		run("Private instance", [&]() { TestPrivateInstance(workspace); });
		if (!failures.empty()) throw std::runtime_error(failures);
		std::cout << "Material importer preparation, ordered publication, dependency retry and GPU parity tests passed\n";
	}
}
