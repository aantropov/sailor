#include "AssetRegistry/Shader/ShaderCache.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Shader/ShaderDependencyFingerprint.h"
#include "AssetRegistry/Shader/ShaderYamlIncludeResolver.h"
#include "FrameGraph/RenderSceneNode.h"
#include "FrameGraph/SkyParameters.h"
#include "FrameGraph/AtmosphericFogNode.h"
#include "FrameGraph/LocalReflection.h"
#include "RHI/GlobalIllumination.h"
#include "RHI/Lighting.h"
#include "RHI/GpuCulling.h"
#include "FrameGraph/DepthPrepassNode.h"
#include "FrameGraph/LightCullingNode.h"
#include "GraphicsDriver/Vulkan/VulkanShaderModule.h"
#include "GraphicsDriver/Vulkan/VulkanPipeline.h"
#include "Workspace/WorkspaceCacheContract.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <spirv_reflect.h>
#include <yaml-cpp/yaml.h>

namespace
{
	using namespace Sailor;
	using namespace Sailor::GraphicsDriver::Vulkan;

	class ShaderLayoutProbe final : public GraphicsDriver::Vulkan::VulkanShaderStage
	{
	public:
		using VulkanShaderStage::ReflectDescriptorSetBindings;
	};

	class LightCullingLayoutProbe final : public Framegraph::LightCullingNode
	{
	public:
		using Constants = PushConstants;
	};

	class FixedShaderSourceStateProvider final : public IShaderSourceStateProvider
	{
	public:
		bool Capture(
			const FileId& uid,
			ShaderSourceState& outState,
			std::string& outDiagnostic) const override
		{
			(void)uid;
			outState.m_timestamp = 100;
			outState.m_fingerprint = 0x5341494c4f525445ull;
			outDiagnostic.clear();
			return true;
		}
	};

	const FixedShaderSourceStateProvider c_shaderSourceStateProvider;

	class CountingShaderSourceStateProvider final : public IShaderSourceStateProvider
	{
	public:
		bool Capture(const FileId& uid, ShaderSourceState& outState, std::string& outDiagnostic) const override
		{
			++captures;
			const bool captured = c_shaderSourceStateProvider.Capture(uid, outState, outDiagnostic);
			outState.m_fingerprint += revision;
			return captured;
		}
		mutable uint32_t captures = 0;
		uint64_t revision = 0;
	};

	class TempDirectory final
	{
	public:
		TempDirectory()
		{
			static uint64_t nextId = 0;
			const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
			m_path = std::filesystem::temp_directory_path() /
				("sailor-shader-cache-artifacts-" + std::to_string(timestamp) + "-" +
					std::to_string(nextId++));
			std::filesystem::create_directories(m_path);
		}

		~TempDirectory() noexcept
		{
			std::error_code error;
			std::filesystem::remove_all(m_path, error);
		}

		std::filesystem::path Path(const char* filename) const
		{
			return m_path / filename;
		}

	private:
		std::filesystem::path m_path;
	};

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	FileId MakeFileId(const std::string& value)
	{
		FileId result;
		result.Deserialize(YAML::Node(value));
		return result;
	}

	TVector<uint32_t> Words(uint32_t seed)
	{
		TVector<uint32_t> result;
		result.Add(0x07230203);
		result.Add(seed);
		result.Add(seed ^ 0x5a5a5a5a);
		result.Add(seed + 17);
		return result;
	}

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(), "test text should be readable: " + path.generic_string());
		return std::string(
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
	}

	size_t CountRegularFiles(const std::filesystem::path& root)
	{
		std::error_code error;
		size_t result = 0;
		for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
			!error && iterator != end;
			iterator.increment(error))
		{
			if (iterator->is_regular_file(error))
			{
				++result;
			}
		}
		Require(!error, "test storage should be enumerable: " + error.message());
		return result;
	}

	void RequireWords(
		const TVector<uint32_t>& actual,
		const TVector<uint32_t>& expected,
		const std::string& context)
	{
		Require(actual.Num() == expected.Num(), context + " should preserve the word count");
		for (size_t index = 0; index < expected.Num(); ++index)
		{
			Require(actual[index] == expected[index], context + " should preserve every word");
		}
	}

	void RequireSpirvCombinedImageSamplerBinding(
		const RHI::ShaderByteCode& byteCode,
		uint32_t set,
		uint32_t binding)
	{
		SpvReflectShaderModule module{};
		Require(
			spvReflectCreateShaderModule(
				byteCode.Num() * sizeof(byteCode[0]),
				&byteCode[0],
				&module) == SPV_REFLECT_RESULT_SUCCESS,
			"compiled shader artifact should support SPIR-V reflection");

		SpvReflectResult result = SPV_REFLECT_RESULT_SUCCESS;
		const SpvReflectDescriptorBinding* reflected =
			spvReflectGetDescriptorBinding(&module, binding, set, &result);
		const bool bMatches =
			result == SPV_REFLECT_RESULT_SUCCESS &&
			reflected &&
			reflected->descriptor_type ==
				SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		spvReflectDestroyShaderModule(&module);

		Require(
			bMatches,
			std::string("compiled shader artifact should expose a combined image sampler at set ") +
				std::to_string(set) +
				", binding " + std::to_string(binding));
	}

	void RequireLocalReflectionUniformLayout(const RHI::ShaderByteCode& byteCode)
	{
		SpvReflectShaderModule module{};
		Require(spvReflectCreateShaderModule(byteCode.Num() * sizeof(uint32_t), byteCode.GetData(),
			&module) == SPV_REFLECT_RESULT_SUCCESS, "local reflection shader should reflect");
		SpvReflectResult status;
		const auto* binding = spvReflectGetDescriptorBinding(&module, 20u, 1u, &status);
		const bool valid = status == SPV_REFLECT_RESULT_SUCCESS && binding &&
			binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
			binding->block.size == sizeof(Framegraph::LocalReflectionParameters) && binding->block.member_count == 3u &&
			binding->block.members[0].offset == offsetof(Framegraph::LocalReflectionParameters, m_positionBlend) &&
			binding->block.members[1].offset == offsetof(Framegraph::LocalReflectionParameters, m_minEnabled) &&
			binding->block.members[2].offset == offsetof(Framegraph::LocalReflectionParameters, m_max);
		spvReflectDestroyShaderModule(&module);
		Require(valid, "local reflection uniform must match the three packed CPU vec4 fields");
	}

	void RequireSpirvStorageImageBinding(
		const RHI::ShaderByteCode& byteCode,
		uint32_t set,
		uint32_t binding)
	{
		SpvReflectShaderModule module{};
		Require(
			spvReflectCreateShaderModule(
				byteCode.Num() * sizeof(byteCode[0]),
				&byteCode[0],
				&module) == SPV_REFLECT_RESULT_SUCCESS,
			"compiled shader artifact should support SPIR-V reflection");

		SpvReflectResult result = SPV_REFLECT_RESULT_SUCCESS;
		const SpvReflectDescriptorBinding* reflected =
			spvReflectGetDescriptorBinding(&module, binding, set, &result);
		const bool bMatches =
			result == SPV_REFLECT_RESULT_SUCCESS &&
			reflected &&
			reflected->descriptor_type ==
				SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		spvReflectDestroyShaderModule(&module);

		Require(
			bMatches,
			std::string("compiled shader artifact should expose a storage image at set ") +
				std::to_string(set) +
				", binding " + std::to_string(binding));
	}

	void RequireSpirvStorageBufferBinding(
		const RHI::ShaderByteCode& byteCode,
		uint32_t set,
		uint32_t binding)
	{
		SpvReflectShaderModule module{};
		Require(
			spvReflectCreateShaderModule(
				byteCode.Num() * sizeof(byteCode[0]),
				&byteCode[0],
				&module) == SPV_REFLECT_RESULT_SUCCESS,
			"compiled shader artifact should support SPIR-V reflection");

		SpvReflectResult result = SPV_REFLECT_RESULT_SUCCESS;
		const SpvReflectDescriptorBinding* reflected =
			spvReflectGetDescriptorBinding(&module, binding, set, &result);
		const bool bMatches =
			result == SPV_REFLECT_RESULT_SUCCESS &&
			reflected &&
			reflected->descriptor_type ==
				SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		spvReflectDestroyShaderModule(&module);

		Require(
			bMatches,
			std::string("compiled shader artifact should expose a storage buffer at set ") +
				std::to_string(set) +
				", binding " + std::to_string(binding));
	}

	void RequireSpirvStorageBufferArrayStride(
		const RHI::ShaderByteCode& byteCode,
		uint32_t set,
		uint32_t binding,
		uint32_t expectedStride)
	{
		SpvReflectShaderModule module{};
		Require(
			spvReflectCreateShaderModule(
				byteCode.Num() * sizeof(byteCode[0]),
				&byteCode[0],
				&module) == SPV_REFLECT_RESULT_SUCCESS,
			"compiled shader artifact should support SPIR-V reflection");

		SpvReflectResult result = SPV_REFLECT_RESULT_SUCCESS;
		const SpvReflectDescriptorBinding* reflected =
			spvReflectGetDescriptorBinding(&module, binding, set, &result);
		uint32_t reflectedStride = 0u;
		if (result == SPV_REFLECT_RESULT_SUCCESS &&
			reflected &&
			reflected->descriptor_type ==
				SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
			reflected->block.member_count > 0u)
		{
			const SpvReflectBlockVariable& reflectedArray =
				reflected->block.members[0];
			reflectedStride = reflectedArray.array.stride;
			if (reflectedStride == 0u && reflectedArray.type_description)
			{
				reflectedStride =
					reflectedArray.type_description->traits.array.stride;
			}
			if (reflectedStride == 0u)
			{
				reflectedStride = reflectedArray.padded_size;
			}
		}
		spvReflectDestroyShaderModule(&module);

		Require(
			reflectedStride == expectedStride,
			std::string("compiled shader storage-buffer stride must match the C++ upload layout at set ") +
				std::to_string(set) + ", binding " + std::to_string(binding) +
				": reflected=" + std::to_string(reflectedStride) +
				", expected=" + std::to_string(expectedStride));
	}

	const RHI::ShaderLayoutBinding* FindBinding(const ShaderLayoutProbe& shader,
		uint32_t setIndex, uint32_t bindingIndex)
	{
		for (const auto& set : shader.GetBindings())
		{
			for (const auto& binding : set)
			{
				if (binding.m_set == setIndex && binding.m_binding == bindingIndex)
				{
					return &binding;
				}
			}
		}
		return nullptr;
	}

	void RequireGltfMaterialLayout(const RHI::ShaderByteCode& byteCode,
		const RHI::ShaderByteCode& reflectionByteCode,
		uint32_t payloadSize, uint32_t stride)
	{
		RequireSpirvStorageBufferArrayStride(byteCode, 3u, 0u, stride);
		ShaderLayoutProbe shader;
		shader.ReflectDescriptorSetBindings(reflectionByteCode);
		const auto* material = FindBinding(shader, 3u, 0u);
		Require(material && material->m_size == payloadSize && material->m_paddedSize == stride,
			"material upload size and array stride must retain internal and trailing std430 padding");
		ShaderLayoutProbe optimizedShader;
		optimizedShader.ReflectDescriptorSetBindings(byteCode);
		const auto* optimizedMaterial = FindBinding(optimizedShader, 3u, 0u);
		Require(optimizedMaterial != nullptr,
			"the optimized shader must retain the used material binding");
		Require(optimizedMaterial->m_size == payloadSize && optimizedMaterial->m_paddedSize == stride,
			"optimized material payload and stride must match the CPU upload layout");
		Require(optimizedMaterial->m_members.Num() == material->m_members.Num(),
			"optimized and reflection shaders must retain the same material fields");
		for (size_t i = 0u; i < optimizedMaterial->m_members.Num(); ++i)
		{
			Require(optimizedMaterial->m_members[i].m_absoluteOffset == material->m_members[i].m_absoluteOffset &&
				optimizedMaterial->m_members[i].m_size == material->m_members[i].m_size,
				"CPU reflection and optimized GPU shader must agree on every material field offset and size");
		}
		auto binding = RHI::RHIShaderBindingPtr::Make();
		binding->SetLayout(*material);
		auto requireMember = [&](const char* name, uint32_t offset, uint32_t size)
		{
			RHI::ShaderLayoutBindingMember member;
			Require(binding->FindVariableInUniformBuffer(name, member) &&
				member.m_absoluteOffset == offset && member.m_size == size,
				std::string("CPU material packing must preserve the reflected offset and size of ") + name);
		};
		requireMember("baseColorFactor", 0u, 16u);
		requireMember("sheenRoughnessFactor", 84u, 4u);
		requireMember("sheenColorFactor", 96u, 16u);
		requireMember("sheenRoughnessSampler", 128u, 4u);
		if (payloadSize == 176u)
		{
			requireMember("transmissionFactor", 132u, 4u);
			requireMember("thicknessFactor", 140u, 4u);
			requireMember("attenuationDistance", 144u, 4u);
			requireMember("indexOfRefraction", 148u, 4u);
			requireMember("thicknessSampler", 152u, 4u);
			requireMember("attenuationColor", 160u, 16u);
		}
		else if (payloadSize == 136u)
		{
			requireMember("indexOfRefraction", 132u, 4u);
		}
	}

	void RequireSkyUniformLayout(const RHI::ShaderByteCode& byteCode)
	{
		SpvReflectShaderModule module{};
		Require(spvReflectCreateShaderModule(byteCode.Num() * sizeof(byteCode[0]),
			&byteCode[0], &module) == SPV_REFLECT_RESULT_SUCCESS,
			"sky shader should support SPIR-V reflection");
		SpvReflectResult result = SPV_REFLECT_RESULT_SUCCESS;
		const auto* binding = spvReflectGetDescriptorBinding(&module, 0u, 1u, &result);
		const std::pair<const char*, size_t> expected[] = {
			{ "lightDirection", offsetof(SkyParameters, m_lightDirection) },
			{ "sunIlluminance", offsetof(SkyParameters, m_sunIlluminance) },
			{ "groundRadiance", offsetof(SkyParameters, m_groundRadiance) },
			{ "cloudsAttenuation1", offsetof(SkyParameters, m_cloudsAttenuation1) },
			{ "cloudsAttenuation2", offsetof(SkyParameters, m_cloudsAttenuation2) },
			{ "cloudsDensity", offsetof(SkyParameters, m_cloudsDensity) },
			{ "cloudsCoverage", offsetof(SkyParameters, m_cloudsCoverage) },
			{ "phaseInfluence1", offsetof(SkyParameters, m_phaseInfluence1) },
			{ "phaseInfluence2", offsetof(SkyParameters, m_phaseInfluence2) },
			{ "eccentrisy1", offsetof(SkyParameters, m_eccentrisy1) },
			{ "eccentrisy2", offsetof(SkyParameters, m_eccentrisy2) },
			{ "fog", offsetof(SkyParameters, m_fog) },
			{ "cloudScatteringScale", offsetof(SkyParameters, m_cloudScatteringScale) },
			{ "ambient", offsetof(SkyParameters, m_ambient) },
			{ "scatteringSteps", offsetof(SkyParameters, m_scatteringSteps) },
			{ "scatteringDensity", offsetof(SkyParameters, m_scatteringDensity) },
			{ "scatteringIntensity", offsetof(SkyParameters, m_scatteringIntensity) },
			{ "scatteringPhase", offsetof(SkyParameters, m_scatteringPhase) },
			{ "sunShaftsIntensity", offsetof(SkyParameters, m_sunShaftsIntensity) },
			{ "sunShaftsDistance", offsetof(SkyParameters, m_sunShaftsDistance) }
		};
		bool matches = result == SPV_REFLECT_RESULT_SUCCESS && binding &&
			binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
			binding->block.member_count == std::size(expected);
		std::string diagnostic = binding ? " memberCount=" + std::to_string(binding->block.member_count) : " missing binding";
		if (matches)
			for (size_t index = 0u; index < std::size(expected); ++index)
			{
				const auto& member = binding->block.members[index];
				// Release SPIR-V strips debug names but retains the full block layout.
				matches &= member.offset == expected[index].second;
				diagnostic += " " + std::string(expected[index].first) + ":" +
					std::to_string(member.offset) + "/" + std::to_string(expected[index].second);
			}
		spvReflectDestroyShaderModule(&module);
		Require(matches, "every compiled sky uniform must match its C++ upload offset;" + diagnostic);
	}

	void RequireSpirvDescriptorBindingAbsent(
		const RHI::ShaderByteCode& byteCode,
		uint32_t set,
		uint32_t binding)
	{
		SpvReflectShaderModule module{};
		Require(
			spvReflectCreateShaderModule(
				byteCode.Num() * sizeof(byteCode[0]),
				&byteCode[0],
				&module) == SPV_REFLECT_RESULT_SUCCESS,
			"compiled shader artifact should support SPIR-V reflection");

		SpvReflectResult result = SPV_REFLECT_RESULT_SUCCESS;
		const SpvReflectDescriptorBinding* reflected =
			spvReflectGetDescriptorBinding(&module, binding, set, &result);
		const bool bAbsent =
			result != SPV_REFLECT_RESULT_SUCCESS || reflected == nullptr;
		spvReflectDestroyShaderModule(&module);

		Require(
			bAbsent,
			std::string("compiled shader artifact must not expose a descriptor at set ") +
				std::to_string(set) +
				", binding " + std::to_string(binding));
	}

	void WriteWords(const std::filesystem::path& path, const TVector<uint32_t>& words)
	{
		std::string diagnostic;
		Require(
			Workspace::AtomicReplaceWorkspaceCacheBinary(
				path,
				words.Num() == 0 ? nullptr : &words[0],
				static_cast<uint64_t>(words.Num()) * sizeof(uint32_t),
				diagnostic),
			"test SPIR-V should be writable: " + diagnostic);
	}

	bool PublishComplete(
		ShaderCache& cache,
		const FileId& uid,
		uint32_t permutation,
		uint32_t seed)
	{
		const TVector<uint32_t> empty;
		return cache.CacheSpirv_ThreadSafe(
			uid,
			permutation,
			Words(seed),
			Words(seed + 1),
			empty,
			Words(seed + 2),
			Words(seed + 3),
			empty);
	}

	template<size_t Size>
	void WriteArtifact(const std::filesystem::path& path, const std::array<uint8_t, Size>& bytes)
	{
		std::string diagnostic;
		Require(
			Workspace::AtomicReplaceWorkspaceCacheBinary(
				path,
				bytes.data(),
				bytes.size(),
				diagnostic),
			"test artifact should be writable: " + diagnostic);
	}

	TVector<uint32_t> SentinelOutput()
	{
		TVector<uint32_t> result;
		result.Add(0xdecafbad);
		result.Add(0xabcdef01);
		return result;
	}

	void RequireSentinel(const TVector<uint32_t>& output, const std::string& context)
	{
		Require(
			output.Num() == 2 && output[0] == 0xdecafbad && output[1] == 0xabcdef01,
			context + " must not partially replace caller output");
	}

	void TestArtifactRoundTrip()
	{
		TempDirectory directory;
		const std::array<uint32_t, 4> words =
		{
			0x07230203,
			0x00010000,
			0x000d000b,
			0x00000001
		};
		const auto metadata = ShaderCache::DescribeArtifact(words.data(), sizeof(words));
		const auto path = directory.Path("valid.spirv");
		std::string writeDiagnostic;
		Require(
			Workspace::AtomicReplaceWorkspaceCacheBinary(
				path,
				words.data(),
				sizeof(words),
				writeDiagnostic),
			"valid SPIR-V fixture should be writable: " + writeDiagnostic);

		TVector<uint32_t> output = SentinelOutput();
		std::string diagnostic;
		Require(
			ShaderCache::ReadSpirvArtifact(path, metadata, output, diagnostic),
			"matching artifact should load: " + diagnostic);
		Require(output.Num() == words.size(), "matching artifact should preserve its word count");
		for (size_t index = 0; index < words.size(); ++index)
		{
			Require(output[index] == words[index], "matching artifact should preserve every word");
		}
	}

	void TestTruncatedArtifactIsRejected()
	{
		TempDirectory directory;
		const std::array<uint8_t, 8> complete = { 3, 2, 35, 7, 0, 0, 1, 0 };
		const std::array<uint8_t, 4> truncated = { 3, 2, 35, 7 };
		const auto metadata = ShaderCache::DescribeArtifact(complete.data(), complete.size());
		const auto path = directory.Path("truncated.spirv");
		WriteArtifact(path, truncated);

		TVector<uint32_t> output = SentinelOutput();
		std::string diagnostic;
		Require(
			!ShaderCache::ReadSpirvArtifact(path, metadata, output, diagnostic),
			"truncated artifact should be rejected");
		Require(diagnostic.find("byte length") != std::string::npos,
			"truncated artifact diagnostic should identify its byte length");
		RequireSentinel(output, "truncated artifact failure");
	}

	void TestMisalignedArtifactIsRejected()
	{
		TempDirectory directory;
		const std::array<uint8_t, 3> bytes = { 3, 2, 35 };
		const auto metadata = ShaderCache::DescribeArtifact(bytes.data(), bytes.size());
		const auto path = directory.Path("misaligned.spirv");
		WriteArtifact(path, bytes);

		TVector<uint32_t> output = SentinelOutput();
		std::string diagnostic;
		Require(
			!ShaderCache::ReadSpirvArtifact(path, metadata, output, diagnostic),
			"misaligned artifact should be rejected");
		Require(diagnostic.find("aligned") != std::string::npos,
			"misaligned artifact diagnostic should identify word alignment");
		RequireSentinel(output, "misaligned artifact failure");
	}

	void TestChecksumMismatchIsRejected()
	{
		TempDirectory directory;
		const std::array<uint8_t, 8> bytes = { 3, 2, 35, 7, 0, 0, 1, 0 };
		auto metadata = ShaderCache::DescribeArtifact(bytes.data(), bytes.size());
		metadata.m_checksum ^= 1;
		const auto path = directory.Path("checksum.spirv");
		WriteArtifact(path, bytes);

		TVector<uint32_t> output = SentinelOutput();
		std::string diagnostic;
		Require(
			!ShaderCache::ReadSpirvArtifact(path, metadata, output, diagnostic),
			"checksum mismatch should be rejected");
		Require(diagnostic.find("checksum mismatch") != std::string::npos,
			"checksum mismatch diagnostic should identify checksum validation");
		RequireSentinel(output, "checksum mismatch failure");
	}

	void TestOwnedArtifactContainment()
	{
		TempDirectory directory;
		const std::filesystem::path cacheRoot = directory.Path("Cache");
		const std::filesystem::path ownedDirectory = cacheRoot / "CompiledShaders";
		std::filesystem::create_directories(ownedDirectory);

		std::string diagnostic;
		Require(
			ShaderCache::ValidateOwnedArtifactPath(
				cacheRoot,
				ownedDirectory,
				ownedDirectory / "direct.spirv",
				diagnostic),
			"a direct child artifact should be accepted: " + diagnostic);

		diagnostic.clear();
		Require(
			!ShaderCache::ValidateOwnedArtifactPath(
				cacheRoot,
				ownedDirectory,
				ownedDirectory / ".." / "neighbor.txt",
				diagnostic),
			"a traversal artifact path should be rejected");
		Require(diagnostic.find("outside owned directory") != std::string::npos,
			"traversal diagnostic should identify the owned-directory boundary");

		const std::filesystem::path symlinkTarget = cacheRoot / "Neighbor";
		std::filesystem::create_directories(symlinkTarget);
		std::filesystem::remove_all(ownedDirectory);
		std::error_code symlinkError;
		std::filesystem::create_directory_symlink(symlinkTarget, ownedDirectory, symlinkError);
		if (!symlinkError)
		{
			diagnostic.clear();
			Require(
				!ShaderCache::ValidateOwnedArtifactPath(
					cacheRoot,
					ownedDirectory,
					ownedDirectory / "linked.spirv",
					diagnostic),
				"a symlinked owned directory should be rejected");
			Require(diagnostic.find("symlinked") != std::string::npos,
				"symlink diagnostic should identify the unsafe directory");
		}
	}

	void TestDebugArtifactsAreRequired()
	{
		TempDirectory directory;
		ShaderCache cache(&c_shaderSourceStateProvider);
		Require(ShaderCacheTestAccess::Configure(cache, directory.Path("Cache")),
			"shader cache test storage should initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-DEBUG-REQUIRED}");

		Require(PublishComplete(cache, uid, 0, 10),
			"a complete regular/debug generation should publish");
		cache.SaveCache(true);
		Require(!cache.IsDirty(), "a complete generation should save before payload validation");

		const std::string payload = ShaderCacheTestAccess::PayloadWithMissingDebug(cache);
		std::string diagnostic;
		Require(!ShaderCacheTestAccess::ParsePayload(payload, diagnostic),
			"payload v1 must reject entries without required debug artifacts");
		Require(diagnostic.find("debug artifacts") != std::string::npos,
			"missing-debug diagnostic should identify the required debug artifact set");

		diagnostic.clear();
		Require(
			!ShaderCacheTestAccess::ParsePayload(
				ShaderCacheTestAccess::PayloadWithMismatchedDebugTopology(cache),
				diagnostic),
			"payload v1 must reject different regular/debug shader stage topology");
		Require(diagnostic.find("identical shader stages") != std::string::npos,
			"topology diagnostic should identify the regular/debug stage mismatch");

		const TVector<uint32_t> empty;
		Require(
			!cache.CacheSpirv_ThreadSafe(
				uid,
				1,
				Words(11),
				Words(12),
				empty,
				empty,
				empty,
				Words(13)),
			"live publication must reject different regular/debug shader stage topology");
	}

	void TestPayloadIgnoresUnknownFields()
	{
		TempDirectory directory;
		ShaderCache cache(&c_shaderSourceStateProvider);
		Require(ShaderCacheTestAccess::Configure(cache, directory.Path("Cache")),
			"shader cache test storage should initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-UNKNOWN-FIELDS}");
		Require(PublishComplete(cache, uid, 0, 19),
			"a complete shader generation should publish");

		std::string diagnostic;
		Require(
			ShaderCacheTestAccess::ParsePayload(
				ShaderCacheTestAccess::PayloadWithUnknownFields(cache),
				diagnostic),
			"unknown engine metadata should not invalidate the shader cache: " + diagnostic);
	}

	void TestWarmPermutationReadsEachArtifactOnce()
	{
		TempDirectory directory;
		CountingShaderSourceStateProvider source;
		ShaderCache cache(&source);
		Require(ShaderCacheTestAccess::Configure(cache, directory.Path("Cache")), "warm-read fixture must initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-SINGLE-READ}");
		Require(PublishComplete(cache, uid, 0, 200), "warm-read graphics fixture must publish");
		cache.SaveCache();
		ShaderCacheTestAccess::TakeArtifactReadCount(cache);
		source.captures = 0;
		ShaderCache::PermutationSpirv loaded;
		Require(cache.TryLoadPermutation(uid, 0, loaded), "both shader variants must load together");
		Require(source.captures == 1, "a warm permutation must capture its dependencies once");
		RequireWords(loaded.m_regular.m_vertex, Words(200), "regular vertex");
		RequireWords(loaded.m_regular.m_fragment, Words(201), "regular fragment");
		RequireWords(loaded.m_debug.m_vertex, Words(202), "debug vertex");
		RequireWords(loaded.m_debug.m_fragment, Words(203), "debug fragment");
		Require(loaded.m_regular.m_compute.IsEmpty() && loaded.m_debug.m_compute.IsEmpty(),
			"graphics permutations must not invent a compute stage");
		const auto reads = ShaderCacheTestAccess::TakeArtifactReadCount(cache);
		Require(reads == 4, "one warm graphics permutation must read four artifacts once, got " + std::to_string(reads));

		const TVector<uint32_t> empty;
		Require(cache.CacheSpirv_ThreadSafe(uid, 1, empty, empty, Words(210), empty, empty, Words(211)),
			"warm-read compute fixture must publish");
		cache.SaveCache();
		ShaderCacheTestAccess::TakeArtifactReadCount(cache);
		source.captures = 0;
		Require(cache.TryLoadPermutation(uid, 1, loaded), "compute permutations must load both variants");
		Require(source.captures == 1, "a warm compute permutation must capture its dependencies once");
		RequireWords(loaded.m_regular.m_compute, Words(210), "regular compute");
		RequireWords(loaded.m_debug.m_compute, Words(211), "debug compute");
		Require(loaded.m_regular.m_vertex.IsEmpty() && loaded.m_regular.m_fragment.IsEmpty() &&
			loaded.m_debug.m_vertex.IsEmpty() && loaded.m_debug.m_fragment.IsEmpty(),
			"a successful compute load must replace preceding graphics output");
		Require(ShaderCacheTestAccess::TakeArtifactReadCount(cache) == 2,
			"one warm compute permutation must read its two artifacts once");
		const auto debugPath = ShaderCacheTestAccess::GetArtifactPath(cache, uid, 1, ShaderCache::ComputeShaderTag, true);
		WriteWords(debugPath, Words(999));
		Require(!cache.TryLoadPermutation(uid, 1, loaded), "corrupt debug bytes must reject the complete permutation");
		RequireWords(loaded.m_regular.m_compute, Words(210), "failed whole-permutation load regular output");
		RequireWords(loaded.m_debug.m_compute, Words(211), "failed whole-permutation load debug output");
		++source.revision;
		ShaderCacheTestAccess::SetArtifactReadIoFailure(cache, true);
		Require(cache.IsExpired(uid, 0) && ShaderCacheTestAccess::IsQuarantined(cache),
			"a stale source must not hide an artifact I/O failure from expiry cleanup");
	}

	void TestReloadKeepsHealthyShaderPermutations()
	{
		for (int damage = 0; damage < 3; ++damage)
		{
			TempDirectory directory;
			ShaderCache cache(&c_shaderSourceStateProvider);
			Require(ShaderCacheTestAccess::Configure(cache, directory.Path("Cache")), "partial recovery fixture must initialize");
			const auto uid = MakeFileId("{SHADER-CACHE-PARTIAL-RECOVERY}");
			const auto other = MakeFileId("{SHADER-CACHE-HEALTHY-NEIGHBOR}");
			Require(PublishComplete(cache, uid, 0, 220) && PublishComplete(cache, uid, 1, 230) &&
				PublishComplete(cache, other, 0, 240), "three independent permutations must publish");
			cache.SaveCache();
			const auto badPath = ShaderCacheTestAccess::GetArtifactPath(cache, uid, 0, ShaderCache::FragmentShaderTag, true);
			const auto healthyPath = ShaderCacheTestAccess::GetArtifactPath(cache, uid, 1, ShaderCache::VertexShaderTag, false);
			const auto healthyGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 1);
			const auto healthyBytes = ReadText(healthyPath);
			if (damage == 0) std::filesystem::remove(badPath);
			else if (damage == 1) std::filesystem::resize_file(badPath, 7);
			else WriteWords(badPath, Words(999));
			cache.LoadCache();
			Require(cache.GetLastLoadResult().IsLoaded() && !cache.IsDirty(),
				"one damaged artifact must not reset an otherwise parseable shader manifest");
			ShaderCache::PermutationSpirv loaded;
			Require(!cache.TryLoadPermutation(uid, 0, loaded), "only the damaged permutation must be removed");
			Require(cache.TryLoadPermutation(uid, 1, loaded), "a healthy sibling permutation must survive reload");
			RequireWords(loaded.m_regular.m_vertex, Words(230), "healthy sibling");
			Require(cache.TryLoadPermutation(other, 0, loaded), "an unrelated healthy shader must survive reload");
			RequireWords(loaded.m_regular.m_vertex, Words(240), "healthy shader");
			Require(ShaderCacheTestAccess::GetGeneration(cache, uid, 1) == healthyGeneration && ReadText(healthyPath) == healthyBytes,
				"recovery must retain existing healthy generations and artifact bytes");
			cache.LoadCache();
			Require(cache.GetLastLoadResult().IsLoaded() && cache.TryLoadPermutation(uid, 1, loaded),
				"the pruned manifest must remain readable after another reload");
		}
	}

	void TestPartialRecoveryRetriesFailedManifestCommit()
	{
		TempDirectory directory;
		ShaderCache cache(&c_shaderSourceStateProvider);
		Require(ShaderCacheTestAccess::Configure(cache, directory.Path("Cache")), "recovery commit fixture must initialize");
		const auto bad = MakeFileId("{SHADER-CACHE-RECOVERY-BAD}");
		const auto good = MakeFileId("{SHADER-CACHE-RECOVERY-GOOD}");
		Require(PublishComplete(cache, bad, 0, 250) && PublishComplete(cache, good, 0, 260),
			"recovery commit fixture must publish");
		cache.SaveCache();
		const auto manifest = ShaderCacheTestAccess::GetCachePath(cache);
		const auto envelope = ReadText(manifest);
		const auto badPath = ShaderCacheTestAccess::GetArtifactPath(cache, bad, 0, ShaderCache::VertexShaderTag, false);
		const auto healthyPath = ShaderCacheTestAccess::GetArtifactPath(cache, good, 0, ShaderCache::VertexShaderTag, false);
		const auto healthyBytes = ReadText(healthyPath);
		WriteWords(badPath, Words(999));
		const auto corruptBytes = ReadText(badPath);
		const auto fileCount = CountRegularFiles(directory.Path("Cache"));
		ShaderCacheTestAccess::FailNextSaveBeforeReplace(cache);
		cache.LoadCache();
		Require(cache.GetLastLoadResult().IsLoaded() && cache.IsDirty() && !cache.Contains(bad),
			"failed recovery commit must leave a usable, retryable filtered cache");
		ShaderCache::PermutationSpirv loaded;
		Require(cache.TryLoadPermutation(good, 0, loaded), "healthy bytecode must remain usable after recovery commit failure");
		RequireWords(loaded.m_regular.m_vertex, Words(260), "healthy bytecode after failed recovery commit");
		Require(ReadText(manifest) == envelope && ReadText(badPath) == corruptBytes &&
			ReadText(healthyPath) == healthyBytes && CountRegularFiles(directory.Path("Cache")) == fileCount,
			"failed recovery commit must not collect or rewrite the old durable files");
		cache.SaveCache();
		Require(!cache.IsDirty() && !std::filesystem::exists(badPath) && ReadText(healthyPath) == healthyBytes,
			"retry must commit the pruned manifest before collecting broken generations");
		cache.LoadCache();
		Require(!cache.Contains(bad) && cache.TryLoadPermutation(good, 0, loaded),
			"a committed recovery must survive reload without recompiling healthy shaders");
	}

	void TestFailedGenerationPreservesDurableGeneration()
	{
		TempDirectory directory;
		const std::filesystem::path cacheRoot = directory.Path("Cache");
		ShaderCache cache(&c_shaderSourceStateProvider);
		Require(ShaderCacheTestAccess::Configure(cache, cacheRoot),
			"shader cache test storage should initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-GENERATION-TRANSACTION}");

		Require(PublishComplete(cache, uid, 3, 20), "the initial generation should publish");
		cache.SaveCache(true);
		Require(!cache.IsDirty(), "the initial generation should be durable");

		const std::string durableGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 3);
		Require(!durableGeneration.empty(), "the durable generation should expose test metadata");
		const auto durableVertexPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			3,
			ShaderCache::VertexShaderTag,
			false);
		const auto cachePath = ShaderCacheTestAccess::GetCachePath(cache);
		const std::string durableEnvelope = ReadText(cachePath);
		const size_t durableFileCount = CountRegularFiles(cacheRoot);

		const TVector<uint32_t> empty;
		std::string diagnostic;
		Require(
			!ShaderCacheTestAccess::PublishWithArtifactFailure(
				cache,
				uid,
				3,
				Words(30),
				Words(31),
				empty,
				Words(32),
				Words(33),
				empty,
				1,
				diagnostic),
			"an injected artifact replacement failure should reject the new generation");
		Require(!diagnostic.empty(), "failed generation publication should report a diagnostic");

		Require(ShaderCacheTestAccess::GetGeneration(cache, uid, 3) == durableGeneration,
			"failed generation publication must retain the durable generation metadata");
		Require(ReadText(cachePath) == durableEnvelope,
			"failed generation publication must not replace the durable envelope");
		Require(std::filesystem::exists(durableVertexPath),
			"failed generation publication must preserve durable artifacts");

		TVector<uint32_t> loadedVertex;
		TVector<uint32_t> loadedFragment;
		TVector<uint32_t> loadedCompute;
		Require(cache.GetSpirvCode(uid, 3, loadedVertex, loadedFragment, loadedCompute),
			"the durable generation should remain readable after failed publication");
		RequireWords(loadedVertex, Words(20), "the durable generation");

		Require(CountRegularFiles(cacheRoot) > durableFileCount,
			"the failure seam should leave an uncommitted immutable artifact for cleanup");
		cache.ClearExpired();
		Require(CountRegularFiles(cacheRoot) == durableFileCount,
			"cleanup should sweep only the uncommitted generation");
		Require(std::filesystem::exists(durableVertexPath),
			"cleanup must whitelist artifacts from the committed metadata snapshot");
		Require(ReadText(cachePath) == durableEnvelope,
			"orphan cleanup should not rewrite unchanged committed metadata");
	}

	void TestRemoveCommitsBeforeGarbageCollection()
	{
		TempDirectory directory;
		ShaderCache cache(&c_shaderSourceStateProvider);
		Require(ShaderCacheTestAccess::Configure(cache, directory.Path("Cache")),
			"shader cache test storage should initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-REMOVE-TRANSACTION}");

		Require(PublishComplete(cache, uid, 0, 40), "the removable generation should publish");
		cache.SaveCache(true);
		const auto artifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			0,
			ShaderCache::VertexShaderTag,
			false);
		const auto cachePath = ShaderCacheTestAccess::GetCachePath(cache);
		const std::string durableEnvelope = ReadText(cachePath);

		std::string diagnostic;
		Require(!ShaderCacheTestAccess::RemoveWithEnvelopeFailure(cache, uid, diagnostic),
			"remove should fail when its metadata replacement is injected to fail");
		Require(cache.Contains(uid), "failed remove must retain its live metadata");
		Require(std::filesystem::exists(artifactPath),
			"failed remove must not garbage-collect a durably referenced artifact");
		Require(ReadText(cachePath) == durableEnvelope,
			"failed remove must preserve the durable envelope");

		ShaderCacheTestAccess::FailNextArtifactSweep(cache);
		cache.Remove(uid);
		Require(!cache.Contains(uid) && cache.IsDirty(),
			"remove should retain retryable dirty state when post-commit cleanup fails");
		Require(std::filesystem::exists(artifactPath),
			"failed post-commit cleanup should retain the now-unreferenced artifact");
		cache.SaveCache();
		Require(!cache.IsDirty() && !std::filesystem::exists(artifactPath),
			"save retry should garbage-collect removed artifacts after the committed metadata");
	}

	void TestExplicitInvalidationSurvivesSameTimestampReload()
	{
		TempDirectory directory;
		const std::filesystem::path cacheRoot = directory.Path("Cache");
		const FileId uid = MakeFileId("{SHADER-CACHE-EXPLICIT-INVALIDATION}");
		std::filesystem::path durableArtifact;
		{
			ShaderCache cache(&c_shaderSourceStateProvider);
			Require(ShaderCacheTestAccess::Configure(cache, cacheRoot),
				"shader cache invalidation storage should initialize");
			Require(PublishComplete(cache, uid, 0, 45),
				"the shader generation to invalidate should publish");
			cache.SaveCache(true);
			durableArtifact = ShaderCacheTestAccess::GetArtifactPath(
				cache,
				uid,
				0,
				ShaderCache::VertexShaderTag,
				false);

			cache.Invalidate(uid);
			Require(cache.Contains(uid) && cache.IsExpired(uid, 0) && !cache.IsDirty(),
				"explicit invalidation should persist an expired entry without removing it");
			Require(std::filesystem::exists(durableArtifact),
				"explicit invalidation should preserve the last durable SPIR-V generation");
		}

		ShaderCache reloaded(&c_shaderSourceStateProvider);
		Require(ShaderCacheTestAccess::Configure(reloaded, cacheRoot),
			"reloaded shader cache invalidation storage should initialize");
		reloaded.LoadCache();
		Require(reloaded.GetLastLoadResult().IsLoaded() && reloaded.IsExpired(uid, 0),
			"explicit invalidation should remain expired after reload at the same source timestamp");
		Require(std::filesystem::exists(durableArtifact),
			"reloading an invalidated entry should retain its last durable artifacts");
	}

	void TestMissingStorageRecoveryDropsStaleArtifactReferences()
	{
		TempDirectory directory;
		const std::filesystem::path cacheRoot = directory.Path("Cache");
		const FileId uid = MakeFileId("{SHADER-CACHE-MISSING-STORAGE}");
		ShaderCache cache(&c_shaderSourceStateProvider);
		Require(ShaderCacheTestAccess::Configure(cache, cacheRoot),
			"shader cache recovery storage should initialize");
		Require(PublishComplete(cache, uid, 0, 46),
			"the shader generation to recover should publish");
		cache.SaveCache(true);
		Require(cache.Contains(uid),
			"the live cache should contain the published generation before deletion");

		std::error_code removeError;
		std::filesystem::remove_all(cacheRoot, removeError);
		Require(!removeError, "the complete shader cache directory should be removable");
		Require(cache.RecoverMissingStorage(),
			"runtime recovery should recreate deleted shader cache storage");
		Require(!cache.Contains(uid) && !cache.IsDirty(),
			"runtime recovery must drop metadata that points to deleted immutable artifacts");
		Require(std::filesystem::is_regular_file(ShaderCacheTestAccess::GetCachePath(cache)) &&
			std::filesystem::is_directory(cacheRoot / "PrecompiledShaders") &&
			std::filesystem::is_directory(cacheRoot / "CompiledShaders") &&
			std::filesystem::is_directory(cacheRoot / "CompiledShadersWithDebug"),
			"runtime recovery should recreate the cache envelope and all owned directories");
	}

	void TestSameSizeChecksumCorruptionAndClearExpiredTransaction()
	{
		TempDirectory directory;
		ShaderCache cache(&c_shaderSourceStateProvider);
		Require(ShaderCacheTestAccess::Configure(cache, directory.Path("Cache")),
			"shader cache test storage should initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-CHECKSUM-EXPIRY}");

		Require(PublishComplete(cache, uid, 1, 50), "the corruption fixture should publish");
		cache.SaveCache(true);
		const auto artifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			1,
			ShaderCache::VertexShaderTag,
			false);
		const auto cachePath = ShaderCacheTestAccess::GetCachePath(cache);
		const std::string durableEnvelope = ReadText(cachePath);
		const std::string corruptGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 1);

		WriteWords(artifactPath, Words(500));
		Require(cache.IsExpired(uid, 1),
			"same-size checksum corruption must expire a cached generation");

		std::string diagnostic;
		Require(!ShaderCacheTestAccess::ClearExpiredWithEnvelopeFailure(cache, diagnostic),
			"expired cleanup should fail when metadata replacement is injected to fail");
		Require(cache.Contains(uid), "failed expired cleanup must retain its live entry");
		Require(std::filesystem::exists(artifactPath),
			"failed expired cleanup must preserve artifacts until metadata commits");
		Require(ReadText(cachePath) == durableEnvelope,
			"failed expired cleanup must preserve the durable envelope");

		Require(PublishComplete(cache, uid, 1, 55),
			"same-size corruption should self-heal through replacement compilation");
		const std::string healedGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 1);
		const auto healedArtifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			1,
			ShaderCache::VertexShaderTag,
			false);
		Require(healedGeneration != corruptGeneration && std::filesystem::exists(artifactPath),
			"self-heal should stage a new generation while retaining the durable corrupt generation");
		cache.SaveCache();
		Require(!cache.IsDirty() && ReadText(cachePath) != durableEnvelope,
			"self-heal should commit its replacement generation");
		Require(!std::filesystem::exists(artifactPath) && std::filesystem::exists(healedArtifactPath),
			"self-heal should sweep the corrupt generation only after replacement commit");
		TVector<uint32_t> vertex;
		TVector<uint32_t> fragment;
		TVector<uint32_t> compute;
		Require(cache.GetSpirvCode(uid, 1, vertex, fragment, compute),
			"self-healed bytecode should be readable");
		RequireWords(vertex, Words(55), "self-healed bytecode");

		const FileId cleanupUid = MakeFileId("{SHADER-CACHE-EXPIRED-CLEANUP}");
		Require(PublishComplete(cache, cleanupUid, 0, 60),
			"the successful expiry-cleanup fixture should publish");
		cache.SaveCache();
		const auto cleanupArtifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			cleanupUid,
			0,
			ShaderCache::VertexShaderTag,
			false);
		WriteWords(cleanupArtifactPath, Words(600));
		ShaderCacheTestAccess::FailNextArtifactSweep(cache);
		cache.ClearExpired();
		Require(!cache.Contains(cleanupUid) && cache.IsDirty() &&
			std::filesystem::exists(cleanupArtifactPath),
			"expired cleanup should remain dirty when post-commit artifact sweeping fails");
		cache.SaveCache();
		Require(!cache.IsDirty() && !std::filesystem::exists(cleanupArtifactPath),
			"save retry should finish expired artifact sweeping after metadata commit");
		Require(cache.Contains(uid), "successful expired cleanup should retain unrelated healed metadata");
	}

	void TestIoFailureQuarantineIsReadOnlyAndSessionOnly()
	{
		TempDirectory directory;
		const std::filesystem::path cacheRoot = directory.Path("Cache");
		ShaderCache cache(&c_shaderSourceStateProvider);
		Require(ShaderCacheTestAccess::Configure(cache, cacheRoot),
			"shader cache test storage should initialize");
		const FileId durableUid = MakeFileId("{SHADER-CACHE-QUARANTINE-DURABLE}");
		const FileId sessionUid = MakeFileId("{SHADER-CACHE-QUARANTINE-SESSION}");

		Require(PublishComplete(cache, durableUid, 0, 60), "the durable quarantine fixture should publish");
		cache.SaveCache(true);
		const auto debugPath = ShaderCacheTestAccess::GetArtifactPath(cache, durableUid, 0, ShaderCache::FragmentShaderTag, true);
		const auto cachePath = ShaderCacheTestAccess::GetCachePath(cache);
		const auto backupPath = cacheRoot / "ShaderCache.backup.yaml";
		std::filesystem::rename(cachePath, backupPath);
		std::filesystem::create_directory(cachePath);
		const std::string durableEnvelope = ReadText(backupPath);
		const size_t preservedFileCount = CountRegularFiles(cacheRoot);

		cache.LoadCache();
		Require(
			cache.GetLastLoadResult().m_status == Workspace::EWorkspaceCacheLoadStatus::IoFailure,
			"an unreadable non-file cache target should enter I/O quarantine");
		Require(ShaderCacheTestAccess::IsQuarantined(cache),
			"I/O failure should make shader cache storage read-only");
		Require(!cache.Contains(durableUid),
			"quarantine should withhold disk-backed metadata that could not be fully reloaded");

		Require(PublishComplete(cache, sessionUid, 2, 70),
			"compilation should remain available as memory-only data during quarantine");
		Require(cache.Contains(sessionUid), "memory-only quarantine publication should be visible in-session");
		Require(!cache.IsDirty(), "memory-only quarantine publication should not claim pending disk state");
		Require(CountRegularFiles(cacheRoot) == preservedFileCount,
			"memory-only quarantine publication must not create disk artifacts");

		TVector<uint32_t> vertex;
		TVector<uint32_t> fragment;
		TVector<uint32_t> compute;
		Require(cache.GetSpirvCode(sessionUid, 2, vertex, fragment, compute, false),
			"regular memory-only SPIR-V should be readable during quarantine");
		RequireWords(vertex, Words(70), "regular quarantine vertex SPIR-V");
		RequireWords(fragment, Words(71), "regular quarantine fragment SPIR-V");
		Require(compute.IsEmpty(), "regular quarantine graphics SPIR-V should not invent compute data");
		Require(cache.GetSpirvCode(sessionUid, 2, vertex, fragment, compute, true),
			"debug memory-only SPIR-V should be readable during quarantine");
		RequireWords(vertex, Words(72), "debug quarantine vertex SPIR-V");
		RequireWords(fragment, Words(73), "debug quarantine fragment SPIR-V");
		Require(compute.IsEmpty(), "debug quarantine graphics SPIR-V should not invent compute data");

		std::filesystem::remove(cachePath);
		cache.LoadCache();
		Require(
			cache.GetLastLoadResult().m_status == Workspace::EWorkspaceCacheLoadStatus::Missing,
			"a missing retry target should not escape an existing I/O quarantine");
		Require(ShaderCacheTestAccess::IsQuarantined(cache),
			"quarantine should persist until a fully successful reload or ClearAll");
		Require(cache.Contains(sessionUid),
			"a failed reload should retain session-only quarantine artifacts");

		cache.SaveCache(true);
		cache.ClearExpired();
		Require(ReadText(backupPath) == durableEnvelope,
			"save and cleanup must preserve unreadable durable storage during quarantine");
		Require(CountRegularFiles(cacheRoot) == preservedFileCount,
			"save and cleanup must not mutate artifact storage during quarantine");

		std::string writeDiagnostic;
		Require(
			Workspace::AtomicReplaceWorkspaceCacheText(
				cachePath,
				"not: [valid",
				writeDiagnostic),
			"a corrupt retry fixture should be writable: " + writeDiagnostic);
		cache.LoadCache();
		Require(
			cache.GetLastLoadResult().m_status == Workspace::EWorkspaceCacheLoadStatus::Corrupt,
			"a corrupt retry target should not escape an existing I/O quarantine");
		Require(ShaderCacheTestAccess::IsQuarantined(cache) && cache.Contains(sessionUid),
			"corrupt reload should retain read-only session state");
		cache.SaveCache(true);
		cache.ClearExpired();
		Require(ReadText(cachePath) == "not: [valid",
			"quarantine must not replace a corrupt retry target without ClearAll");

		std::filesystem::remove(cachePath);
		std::filesystem::rename(backupPath, cachePath);
		WriteWords(debugPath, Words(999));
		const auto badBytes = ReadText(debugPath);
		cache.LoadCache();
		Require(ShaderCacheTestAccess::IsQuarantined(cache) && cache.Contains(sessionUid) &&
			cache.GetLastLoadResult().m_status == Workspace::EWorkspaceCacheLoadStatus::Corrupt,
			"partial artifact recovery must not escape an existing I/O quarantine");
		cache.SaveCache(true);
		cache.ClearExpired();
		Require(ReadText(cachePath) == durableEnvelope && ReadText(debugPath) == badBytes,
			"a valid manifest with bad artifacts must stay read-only during quarantine");
		WriteWords(debugPath, Words(63));
		cache.LoadCache();
		Require(cache.GetLastLoadResult().IsLoaded(),
			"a later full successful reload should leave I/O quarantine");
		Require(!ShaderCacheTestAccess::IsQuarantined(cache),
			"successful full reload should restore persistent cache access");
		Require(cache.Contains(durableUid), "successful reload should restore durable metadata");
		Require(!cache.Contains(sessionUid),
			"successful reload should discard session-only quarantine artifacts");
	}

	void TestRuntimeArtifactIoFailureEntersReadOnlyQuarantine()
	{
		TempDirectory directory;
		const std::filesystem::path cacheRoot = directory.Path("Cache");
		ShaderCache cache(&c_shaderSourceStateProvider);
		Require(ShaderCacheTestAccess::Configure(cache, cacheRoot),
			"shader cache test storage should initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-RUNTIME-IO-QUARANTINE}");

		Require(PublishComplete(cache, uid, 0, 80), "the runtime I/O fixture should publish");
		cache.SaveCache(true);
		const auto cachePath = ShaderCacheTestAccess::GetCachePath(cache);
		const auto artifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			0,
			ShaderCache::VertexShaderTag,
			false);
		const std::string durableEnvelope = ReadText(cachePath);
		const std::string durableArtifact = ReadText(artifactPath);
		const std::string durableGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 0);
		const size_t durableFileCount = CountRegularFiles(cacheRoot);

		ShaderCacheTestAccess::SetArtifactReadIoFailure(cache, true);
		Require(cache.IsExpired(uid, 0),
			"runtime artifact I/O failure should force a session recompile");
		ShaderCacheTestAccess::SetArtifactReadIoFailure(cache, false);
		Require(ShaderCacheTestAccess::IsQuarantined(cache),
			"runtime artifact I/O failure should enter read-only storage quarantine");
		Require(
			cache.GetLastLoadResult().m_status == Workspace::EWorkspaceCacheLoadStatus::IoFailure,
			"runtime artifact I/O failure should publish an I/O load status");
		Require(!cache.Contains(uid),
			"runtime I/O quarantine should withhold the unread disk-backed entry");

		Require(PublishComplete(cache, uid, 0, 90),
			"runtime I/O quarantine should accept session-only recompiled bytecode");
		Require(cache.Contains(uid) && !cache.IsDirty(),
			"session-only recompile should be visible without pending disk mutation");
		TVector<uint32_t> vertex;
		TVector<uint32_t> fragment;
		TVector<uint32_t> compute;
		Require(cache.GetSpirvCode(uid, 0, vertex, fragment, compute),
			"session-only recompiled bytecode should be readable");
		RequireWords(vertex, Words(90), "runtime quarantine bytecode");

		cache.SaveCache(true);
		cache.ClearExpired();
		Require(ReadText(cachePath) == durableEnvelope,
			"runtime I/O quarantine must preserve durable metadata byte-for-byte");
		Require(ReadText(artifactPath) == durableArtifact,
			"runtime I/O quarantine must preserve durable artifacts byte-for-byte");
		Require(CountRegularFiles(cacheRoot) == durableFileCount,
			"runtime I/O quarantine must not create or collect disk files");

		cache.LoadCache();
		Require(cache.GetLastLoadResult().IsLoaded() &&
			!ShaderCacheTestAccess::IsQuarantined(cache),
			"a full successful reload should leave runtime I/O quarantine");
		Require(ShaderCacheTestAccess::GetGeneration(cache, uid, 0) == durableGeneration,
			"successful reload should restore the durable immutable generation");
		Require(cache.GetSpirvCode(uid, 0, vertex, fragment, compute),
			"restored durable bytecode should be readable");
		RequireWords(vertex, Words(80), "restored durable bytecode");
	}

	void TestFailedGlslCompilationPreservesBytecode()
	{
		const std::string validSource =
			"#version 450\nlayout(location = 0) out vec4 color;\nvoid main() { color = vec4(1.0); }\n";
		struct InvalidShader
		{
			const char* m_filename;
			std::string m_source;
		};
		const InvalidShader invalidShaders[] = {
			{ "single-digit.frag", "#version 450\n#error invalid shader\nvoid main() {}\n" },
			{ "C:\\Project With Spaces\\Shaders\\invalid.frag", "#version 450\n" + std::string(12, '\n') + "#error invalid shader\nvoid main() {}\n" },
			{ "multiple-errors.frag", "#version 450\n#error first diagnostic\n#error second diagnostic\nvoid main() {}\n" },
			{ "remapped-line.frag", "#version 450\n#line 1200\n#error remapped diagnostic\nvoid main() {}\n" },
			{ "", "#version 450\nvoid main() { invalid_expression; }\n" }
		};
		for (bool debug : { false, true })
		{
			RHI::ShaderByteCode byteCode;
			Require(ShaderCompilerTestAccess::CompileGlslToSpirv("valid.frag", validSource,
				RHI::EShaderStage::Fragment, byteCode, debug), "the fixture must compile real valid GLSL");
			const RHI::ShaderByteCode previous = byteCode;
			Require(!previous.IsEmpty(), "the fixture must retain actual compiled SPIR-V");
			for (const InvalidShader& shader : invalidShaders)
			{
				Require(!ShaderCompilerTestAccess::CompileGlslToSpirv(shader.m_filename, shader.m_source,
					RHI::EShaderStage::Fragment, byteCode, debug), "invalid GLSL must return failure without throwing while reading its diagnostics");
				RequireWords(byteCode, previous, "a failed compile must not replace the previously compiled bytecode");
			}
			Require(ShaderCompilerTestAccess::CompileGlslToSpirv("recovered.frag", validSource,
				RHI::EShaderStage::Fragment, byteCode, debug), "valid GLSL must still compile after diagnostic failures");
		}
	}

	void TestShaderCompilerFailureLifecycle()
	{
		const FileId parsedOnly = MakeFileId("{SHADER-DEPENDENCY-PARSED}");
		const FileId shared = MakeFileId("{SHADER-DEPENDENCY-SHARED}");
		const FileId loadedOnly = MakeFileId("{SHADER-DEPENDENCY-LOADED}");
		const TVector<FileId> dependencyCandidates =
			ShaderCompilerTestAccess::MergeShaderDependencyCandidates(
				{ parsedOnly, shared },
				{ shared, loadedOnly });
		Require(
			dependencyCandidates.Num() == 3 &&
			dependencyCandidates.Contains(parsedOnly) &&
			dependencyCandidates.Contains(shared) &&
			dependencyCandidates.Contains(loadedOnly),
			"GLSL dependency propagation should merge parsed and loaded shader ids without duplicates");
		Require(
			ShaderCompilerTestAccess::NormalizeShaderExtension("Shaders/Upper.SHADER") == "shader" &&
			ShaderCompilerTestAccess::NormalizeShaderExtension("Shaders/Upper.GLSL") == "glsl",
			"shader hot reload should match registered extensions case-insensitively");
		Require(
			ShaderCompilerTestAccess::DoesShaderIncludePath(
				{ "Shaders/Shared/Common.glsl" },
				"Shaders/Shared/./Common.glsl"),
			"shader dependency matching should normalize virtual paths");
#if defined(_WIN32)
		Require(
			ShaderCompilerTestAccess::DoesShaderIncludePath(
				{ "Shaders\\Shared\\Common.GLSL" },
				"shaders/SHARED/common.glsl"),
			"Windows shader dependency matching should normalize separators and path case");
#endif

		const bool allSucceeded[] = { true, true, true };
		const bool oneFailed[] = { true, false, true };
		Require(
			ShaderCompilerTestAccess::AggregateCompileResults(allSucceeded, std::size(allSucceeded)),
			"compile aggregation should retain all-success results");
		Require(
			!ShaderCompilerTestAccess::AggregateCompileResults(oneFailed, std::size(oneFailed)),
			"one failed permutation should fail the aggregate compile result");
		Require(ShaderCompilerTestAccess::ShouldRetryCacheSave(0, true),
			"current bytecode with dirty metadata should select the save-only retry branch");
		Require(!ShaderCompilerTestAccess::ShouldRetryCacheSave(0, false) &&
			!ShaderCompilerTestAccess::ShouldRetryCacheSave(1, true),
			"save-only retry should require both no compilation work and dirty metadata");
		Require(ShaderCompilerTestAccess::ExerciseFailedLoadEvictionAndRetry(),
			"failed shader load eviction should permit a second permutation load attempt");
		Require(ShaderCompilerTestAccess::ExercisePromiseGarbageCollection(),
			"promise garbage collection should retain newly pending permutations");

		TempDirectory directory;
		const std::filesystem::path cacheRoot = directory.Path("Cache");
		ShaderCache cache(&c_shaderSourceStateProvider);
		Require(ShaderCacheTestAccess::Configure(cache, cacheRoot),
			"shader cache test storage should initialize");
		const FileId uid = MakeFileId("{SHADER-CACHE-SAVE-RETRY}");

		Require(PublishComplete(cache, uid, 0, 100), "the first save generation should publish");
		cache.SaveCache(true);
		const auto cachePath = ShaderCacheTestAccess::GetCachePath(cache);
		const auto oldArtifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			0,
			ShaderCache::VertexShaderTag,
			false);
		const std::string oldGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 0);
		const std::string oldEnvelope = ReadText(cachePath);

		Require(PublishComplete(cache, uid, 0, 110), "the replacement generation should publish");
		const auto newArtifactPath = ShaderCacheTestAccess::GetArtifactPath(
			cache,
			uid,
			0,
			ShaderCache::VertexShaderTag,
			false);
		const std::string newGeneration = ShaderCacheTestAccess::GetGeneration(cache, uid, 0);
		Require(oldGeneration != newGeneration && cache.IsDirty(),
			"replacement publication should remain dirty under a new immutable generation");
		Require(std::filesystem::exists(oldArtifactPath) && std::filesystem::exists(newArtifactPath),
			"both generations should exist before metadata commit");

		ShaderCacheTestAccess::FailNextSaveBeforeReplace(cache);
		Require(
			!ShaderCompilerTestAccess::SaveCacheAndCombineResult(cache, true),
			"compile orchestration should report the first injected save failure");
		Require(cache.IsDirty() && ReadText(cachePath) == oldEnvelope,
			"failed save should retain dirty state and the old durable envelope");
		Require(std::filesystem::exists(oldArtifactPath) && std::filesystem::exists(newArtifactPath),
			"failed save must retain both the durable and uncommitted generations");

		Require(ShaderCompilerTestAccess::SaveCacheAndCombineResult(cache, true),
			"a no-recompile save retry should publish the already compiled generation");
		Require(!cache.IsDirty() && ReadText(cachePath) != oldEnvelope,
			"successful retry should commit the new envelope and clear dirty state");
		Require(!std::filesystem::exists(oldArtifactPath) && std::filesystem::exists(newArtifactPath),
			"post-commit GC should remove only the old immutable generation");
		Require(!ShaderCompilerTestAccess::SaveCacheAndCombineResult(cache, false),
			"a compile failure should remain failed even when cache persistence succeeds");
	}

	ShaderDependencyFile Dependency(
		const std::string& virtualPath,
		const std::string& winnerIdentity,
		int64_t modificationTimeNanoseconds,
		uint32_t mountKind)
	{
		ShaderDependencyFile dependency;
		dependency.m_virtualPath = virtualPath;
		dependency.m_winnerIdentity = winnerIdentity;
		dependency.m_revision.m_modificationTimeNanoseconds = modificationTimeNanoseconds;
		dependency.m_revision.m_bIsValid = true;
		dependency.m_mountKind = mountKind;
		return dependency;
	}

	void TestShaderDependencyFingerprintTracksTimestampAndWinner()
	{
		TVector<ShaderDependencyFile> baselineDependencies;
		baselineDependencies.Add(Dependency(
			"Shaders/User.shader",
			"/Engine/Content/Shaders/User.shader",
			5000000000ll,
			0));
		baselineDependencies.Add(Dependency(
			"Shaders/Library/Math.glsl",
			"/Workspace/Content/Shaders/Library/Math.glsl",
			6000000000ll,
			1));

		const uint64_t baseline = CalculateShaderDependencyFingerprint(baselineDependencies);
		Require(baseline != 0 &&
			CalculateShaderDependencyFingerprint(baselineDependencies) == baseline,
			"identical shader dependency snapshots should have a stable non-zero fingerprint");

		TVector<ShaderDependencyFile> laterEdit = baselineDependencies;
		laterEdit[1].m_revision.m_modificationTimeNanoseconds += 1000000000ll;
		Require(CalculateShaderDependencyFingerprint(laterEdit) != baseline,
			"a changed GLSL timestamp should invalidate the shader fingerprint");

		TVector<ShaderDependencyFile> backdatedEdit = baselineDependencies;
		backdatedEdit[1].m_revision.m_modificationTimeNanoseconds = 1000000000ll;
		Require(CalculateShaderDependencyFingerprint(backdatedEdit) != baseline,
			"backdated GLSL edits should invalidate the shader fingerprint");

		TVector<ShaderDependencyFile> engineFallback = baselineDependencies;
		engineFallback[1].m_winnerIdentity = "/Engine/Content/Shaders/Library/Math.glsl";
		engineFallback[1].m_mountKind = 0;
		Require(CalculateShaderDependencyFingerprint(engineFallback) != baseline,
			"workspace-winner to Engine-fallback transitions should invalidate the shader fingerprint");
	}

	void TestMissingYamlIncludeFailsWithoutPartialSource()
	{
		TVector<std::string> includes;
		includes.Add("Shaders/Library/Math.glsl");
		std::string source = "#define VERTEX\n";
		std::string diagnostic;
		Require(!ShaderYamlIncludeResolver::Append(
				includes,
				[](const std::string&, std::string&) { return false; },
				source,
				diagnostic),
			"an unresolved YAML shader include should fail source generation");
		Require(source == "#define VERTEX\n",
			"a missing YAML include should not publish partially generated shader source");
		Require(diagnostic.find("Shaders/Library/Math.glsl") != std::string::npos &&
			diagnostic.find("Content-root virtual paths") != std::string::npos,
			"a missing YAML include should report its Content-root virtual path contract");

		std::string resolvedSource;
		Require(ShaderYamlIncludeResolver::Append(
				includes,
				[](const std::string& include, std::string& contents)
				{
					if (include != "Shaders/Library/Math.glsl")
					{
						return false;
					}
					contents = "#include \"Nested.glsl\"\nvec3 mathLibrary;";
					return true;
				},
				resolvedSource,
				diagnostic),
			"a resolved YAML include should append its library source");
		Require(resolvedSource == "#include \"Nested.glsl\"\nvec3 mathLibrary;\n",
			"native nested GLSL include text should remain opaque to the YAML include resolver");
	}

	RHI::ShaderByteCode CompileRuntimeShaderStage(const char* shaderPath,
		std::initializer_list<const char*> permutationDefines, RHI::EShaderStage stage,
		bool bIsDebug = false)
	{
		const std::filesystem::path contentRoot =
			std::filesystem::path(SAILOR_TEST_SOURCE_DIR) / "Content";
		ShaderAsset shader;
		shader.Deserialize(YAML::Load(ReadText(contentRoot / shaderPath)));
		const char* stageDefine = stage == RHI::EShaderStage::Vertex ? "VERTEX" :
			(stage == RHI::EShaderStage::Fragment ? "FRAGMENT" : "COMPUTE");
		std::string source = shader.GetGlslCommonCode() + "\n#define " + stageDefine + "\n";
		for (const char* define : permutationDefines)
		{
			source += std::string("#define ") + define + "\n";
		}
#if defined(__APPLE__)
		if (stage != RHI::EShaderStage::Compute)
		{
			source += "#define SAILOR_TEXTURE_REMAP\n";
		}
#endif
		std::string diagnostic;
		Require(ShaderYamlIncludeResolver::Append(shader.GetIncludes(),
			[&](const std::string& include, std::string& contents)
			{
				std::ifstream input(contentRoot / include, std::ios::binary);
				if (!input.is_open()) return false;
				contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
				return true;
			}, source, diagnostic),
			std::string("runtime shader includes should resolve for ") + shaderPath + ": " + diagnostic);
		source += stage == RHI::EShaderStage::Vertex ? shader.GetGlslVertexCode() :
			(stage == RHI::EShaderStage::Fragment ? shader.GetGlslFragmentCode() : shader.GetGlslComputeCode());
		RHI::ShaderByteCode byteCode;
		Require(ShaderCompilerTestAccess::CompileGlslToSpirv(shaderPath, source, stage, byteCode, bIsDebug),
			std::string("runtime shader stage should compile: ") + shaderPath + " " + stageDefine);
		Require(!byteCode.IsEmpty(), "runtime shader stage should produce SPIR-V");
		return byteCode;
	}

	VulkanShaderStagePtr ReflectStage(const RHI::ShaderByteCode& code, RHI::EShaderStage stage)
	{
		auto reflected = TRefPtr<ShaderLayoutProbe>::Make();
		reflected->m_stage = static_cast<VkShaderStageFlagBits>(stage);
		reflected->ReflectDescriptorSetBindings(code);
		return reflected;
	}

	TVector<VkPushConstantRange> RequirePushConstantLayout(const TVector<VulkanShaderStagePtr>& stages,
		VkShaderStageFlags flags, uint32_t offset, uint32_t size)
	{
		TVector<VkPushConstantRange> ranges;
		Require(VulkanPipelineLayout::BuildPushConstantRanges(stages, 128u, ranges),
			"compiled shader push constants must fit a 128-byte device");
		if (size == 0u)
		{
			Require(ranges.IsEmpty(), "a shader without push constants must produce no native range");
		}
		else
		{
			Require(ranges.Num() == 1u && ranges[0].stageFlags == flags &&
				ranges[0].offset == offset && ranges[0].size == size,
				"the native layout must preserve compiled stage visibility and the exact byte span");
		}
		return ranges;
	}

	void TestCompiledPushConstantRanges()
	{
		const struct
		{
			RHI::EShaderStage m_stage;
			uint32_t m_offset;
			uint32_t m_size;
			const char* m_source;
		} cases[] = {
			{ RHI::EShaderStage::Compute, 0u, 4u, R"(#version 450
layout(local_size_x=1) in;
layout(push_constant) uniform Params { uint value; } params;
layout(set=0,binding=0) buffer Result { uint value; } result;
void main() { result.value = params.value; }
)" },
			{ RHI::EShaderStage::Compute, 16u, 4u, R"(#version 450
layout(local_size_x=1) in;
layout(push_constant) uniform Params { layout(offset=16) uint value; } params;
layout(set=0,binding=0) buffer Result { uint value; } result;
void main() { result.value = params.value; }
)" },
			{ RHI::EShaderStage::Vertex, 64u, 64u, R"(#version 450
layout(location=0) in vec4 position;
layout(push_constant) uniform Params { layout(offset=64) mat4 value; } params;
void main() { gl_Position = params.value * position; }
)" },
			{ RHI::EShaderStage::Fragment, 0u, 4u, R"(#version 450
layout(push_constant) uniform Params { uint value; } params;
layout(location=0) out vec4 color;
void main() { color = vec4(float(params.value)); }
)" },
			{ RHI::EShaderStage::Vertex, 0u, 0u, R"(#version 450
layout(location=0) in vec4 position;
void main() { gl_Position = position; }
)" }
		};
		for (const bool debug : { false, true })
		{
			TVector<VulkanShaderStagePtr> stages;
			for (const auto& test : cases)
			{
				RHI::ShaderByteCode code;
				Require(ShaderCompilerTestAccess::CompileGlslToSpirv("push-constants.glsl",
					test.m_source, test.m_stage, code, debug), "push constant fixture must compile");
				auto reflected = ReflectStage(code, test.m_stage);
				RequirePushConstantLayout({ reflected }, static_cast<VkShaderStageFlags>(test.m_stage),
					test.m_offset, test.m_size);
				stages.Add(reflected);
			}
			RequirePushConstantLayout({ stages[4], stages[3] }, VK_SHADER_STAGE_FRAGMENT_BIT, 0u, 4u);
			RequirePushConstantLayout({ stages[2], stages[3] },
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0u, 128u);
		}
	}

	void TestRuntimePushConstantLayouts()
	{
		for (const bool debug : { false, true })
		{
			const auto graphics = [&](const char* path, std::initializer_list<const char*> defines)
			{
				return TVector<VulkanShaderStagePtr>{
					ReflectStage(CompileRuntimeShaderStage(path, defines, RHI::EShaderStage::Vertex, debug),
						RHI::EShaderStage::Vertex),
					ReflectStage(CompileRuntimeShaderStage(path, defines, RHI::EShaderStage::Fragment, debug),
						RHI::EShaderStage::Fragment) };
			};
			RequirePushConstantLayout(graphics("Shaders/Sky.shader", { "CLOUDS", "DITHER" }),
				debug ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_FRAGMENT_BIT,
				0u, 4u);
			RequirePushConstantLayout(graphics("Shaders/ImGuiUI.shader", {}), VK_SHADER_STAGE_VERTEX_BIT, 0u, 16u);

			const auto lights = ReflectStage(CompileRuntimeShaderStage("Shaders/ComputeLightCulling.shader", {},
				RHI::EShaderStage::Compute, debug), RHI::EShaderStage::Compute);
			using Constants = LightCullingLayoutProbe::Constants;
			constexpr uint32_t memberEnd = offsetof(Constants, m_lightsNum) + sizeof(int32_t);
			auto layout = VulkanPipelineLayoutPtr::Make();
			layout->m_pushConstantRanges = RequirePushConstantLayout({ lights }, VK_SHADER_STAGE_COMPUTE_BIT, 0u, memberEnd);
			Constants constants{};
			const void* data = &constants;
			VkPushConstantRange update;
			Require(sizeof(constants) > memberEnd &&
				layout->GetPushConstantUpdate(0u, sizeof(constants), data, update) &&
				update.size == memberEnd && data == &constants,
				"LightCulling must submit its last member without trailing C++ alignment padding");
		}
	}

	void TestRuntimeLightingShadersCompile()
	{
		const std::array<const char*, 5> shaderPaths =
		{
			"Shaders/Standard.shader",
			"Shaders/Standard_glTF.shader",
			"Shaders/Landscape.shader",
			"Shaders/HBAO.shader",
			"Shaders/HBAO_Blur.shader"
		};

		auto compileRuntimeFragment = [](
			const char* shaderPath,
			std::initializer_list<const char*> permutationDefines,
			bool bIsDebug = false)
		{
			return CompileRuntimeShaderStage(
				shaderPath,
				permutationDefines,
				RHI::EShaderStage::Fragment,
				bIsDebug);
		};
		auto compileRuntimeVertex = [](
			const char* shaderPath,
			std::initializer_list<const char*> permutationDefines)
		{
			return CompileRuntimeShaderStage(
				shaderPath,
				permutationDefines,
				RHI::EShaderStage::Vertex);
		};

		const auto fogByteCode = compileRuntimeFragment("Shaders/AtmosphericFog.shader", {});
		compileRuntimeVertex("Shaders/AtmosphericFog.shader", {});
		RequireSpirvCombinedImageSamplerBinding(fogByteCode, 1u, 1u);
		RequireSpirvCombinedImageSamplerBinding(fogByteCode, 1u, 2u);
		RequireSpirvCombinedImageSamplerBinding(fogByteCode, 1u, 3u);
		RequireSpirvDescriptorBindingAbsent(fogByteCode, 1u, 4u);
		SpvReflectShaderModule fogModule{};
		Require(spvReflectCreateShaderModule(fogByteCode.Num() * sizeof(uint32_t), fogByteCode.GetData(),
			&fogModule) == SPV_REFLECT_RESULT_SUCCESS, "Fog shader must support reflection");
		SpvReflectResult fogStatus;
		const auto* fogBinding = spvReflectGetDescriptorBinding(&fogModule, 0u, 1u, &fogStatus);
		const bool fogLayoutValid = fogStatus == SPV_REFLECT_RESULT_SUCCESS && fogBinding &&
			fogBinding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
			fogBinding->block.size == sizeof(Framegraph::AtmosphericFogNode::ShaderParameters) && fogBinding->block.member_count == 6 &&
			fogBinding->block.members[0].offset == 0 && fogBinding->block.members[1].offset == 16 &&
			fogBinding->block.members[2].offset == offsetof(Framegraph::AtmosphericFogNode::ShaderParameters, m_directionToSun) &&
			fogBinding->block.members[3].offset == offsetof(Framegraph::AtmosphericFogNode::ShaderParameters, m_sunIlluminance) &&
			fogBinding->block.members[4].offset == offsetof(Framegraph::AtmosphericFogNode::ShaderParameters, m_previousDirectionToSun) &&
			fogBinding->block.members[5].offset == offsetof(Framegraph::AtmosphericFogNode::ShaderParameters, m_previousSunIlluminance) &&
			fogModule.push_constant_block_count == 0 && fogModule.output_variable_count == 1;
		spvReflectDestroyShaderModule(&fogModule);
		Require(fogLayoutValid, "Fog shader must match the CPU medium/lighting snapshots and one colour output");

		for (size_t shaderIndex = 0u;
			shaderIndex < shaderPaths.size();
			++shaderIndex)
		{
			const RHI::ShaderByteCode byteCode =
				compileRuntimeFragment(shaderPaths[shaderIndex], {});
			if (shaderIndex == 1u)
				RequireGltfMaterialLayout(byteCode,
					compileRuntimeFragment(shaderPaths[shaderIndex], {}, true), 132u, 144u);
			if (shaderIndex < 3u)
			{
				RequireLocalReflectionUniformLayout(byteCode);
				RequireSpirvCombinedImageSamplerBinding(byteCode, 1u, 21u);
				RequireSpirvStorageBufferArrayStride(
					byteCode,
					1u,
					0u,
					sizeof(RHI::RHILightShaderData));
				const RHI::ShaderByteCode vertexByteCode =
					compileRuntimeVertex(shaderPaths[shaderIndex], {});
				RequireSpirvStorageBufferBinding(vertexByteCode, 2u, 0u);
				RequireSpirvStorageBufferBinding(vertexByteCode, 2u, 1u);
				RequireSpirvStorageBufferArrayStride(
					vertexByteCode,
					2u,
					0u,
					sizeof(Framegraph::RenderSceneNode::PerInstanceData));
				RequireSpirvStorageBufferArrayStride(
					vertexByteCode,
					2u,
					1u,
					sizeof(uint32_t));
				RequireSpirvStorageBufferBinding(byteCode, 1u, 12u);
				RequireSpirvDescriptorBindingAbsent(byteCode, 1u, 13u);
				RequireSpirvStorageBufferBinding(byteCode, 1u, 14u);
				for (uint32_t binding = 15u; binding <= 17u; ++binding)
				{
					RequireSpirvStorageBufferBinding(byteCode, 1u, binding);
				}
				RequireSpirvStorageBufferArrayStride(
					byteCode,
					1u,
					14u,
					sizeof(RHI::RHIGlobalIlluminationGpuBrick));
				RequireSpirvStorageBufferArrayStride(
					byteCode,
					1u,
					15u,
					sizeof(RHI::RHIGlobalIlluminationGpuProbe));
				RequireSpirvStorageBufferArrayStride(
					byteCode,
					1u,
					16u,
					sizeof(RHI::RHIGlobalIlluminationGpuCoefficients));
				RequireSpirvStorageBufferArrayStride(
					byteCode,
					1u,
					17u,
					sizeof(RHI::RHIGlobalIlluminationGpuState));
				RequireSpirvCombinedImageSamplerBinding(
					byteCode,
					2u,
					4u);
				RequireSpirvDescriptorBindingAbsent(byteCode, 1u, 18u);
				RequireSpirvDescriptorBindingAbsent(byteCode, 1u, 19u);
				RequireSpirvDescriptorBindingAbsent(byteCode, 1u, 22u);
			}
		}
		compileRuntimeFragment(
			"Shaders/Standard.shader",
			{ "ALPHA_CUTOUT" });
		compileRuntimeFragment(
			"Shaders/Standard_glTF.shader",
			{ "ALPHA_CUTOUT" });
		const RHI::ShaderByteCode materialExtensionsByteCode =
			compileRuntimeFragment(
				"Shaders/Standard_glTF.shader",
				{ "CLEAR_COAT", "SHEEN", "TRANSMISSION" });
		RequireSpirvCombinedImageSamplerBinding(
			materialExtensionsByteCode,
			1u,
			19u);
		RequireLocalReflectionUniformLayout(materialExtensionsByteCode);
		for (const bool debug : { false, true })
		{
			const auto transmission = compileRuntimeFragment("Shaders/Standard_glTF.shader", { "TRANSMISSION" }, debug);
			RequireSpirvCombinedImageSamplerBinding(transmission, 2u, 2u);
			RequireSpirvCombinedImageSamplerBinding(transmission, 2u, 4u);
			RequireSpirvDescriptorBindingAbsent(transmission, 1u, 10u);
			RequireSpirvDescriptorBindingAbsent(transmission, 1u, 18u);
		}
		RequireGltfMaterialLayout(materialExtensionsByteCode, compileRuntimeFragment(
			"Shaders/Standard_glTF.shader", { "CLEAR_COAT", "SHEEN", "TRANSMISSION" }, true), 176u, 176u);
		RequireGltfMaterialLayout(compileRuntimeFragment(
			"Shaders/Standard_glTF.shader", { "TRANSMISSION", "MOTIONS" }), compileRuntimeFragment(
			"Shaders/Standard_glTF.shader", { "TRANSMISSION", "MOTIONS" }, true), 176u, 176u);
		RequireSpirvCombinedImageSamplerBinding(materialExtensionsByteCode, 1u, 21u);
		RequireSpirvCombinedImageSamplerBinding(materialExtensionsByteCode, 1u, 22u);
		RequireGltfMaterialLayout(compileRuntimeFragment(
			"Shaders/Standard_glTF.shader",
			{ "MATERIAL_IOR" }), compileRuntimeFragment(
			"Shaders/Standard_glTF.shader", { "MATERIAL_IOR" }, true), 136u, 144u);
		compileRuntimeVertex(
			"Shaders/Standard_glTF.shader",
			{ "SKINNING", "TRANSMISSION" });
		compileRuntimeFragment(
			"Shaders/Standard_glTF.shader",
			{ "DISABLE_SCREEN_SPACE_AO" });
		for (const auto* path : { "Shaders/Standard.shader", "Shaders/Standard_glTF.shader", "Shaders/Landscape.shader", "Shaders/Unlit.shader", "Shaders/Simple.shader" })
		{
			const auto vertex = compileRuntimeVertex(path, { "MOTIONS" });
			RequireSpirvStorageBufferArrayStride(vertex, 2u, 0u, sizeof(Framegraph::RenderSceneNode::PerInstanceData));
			for (const bool motions : { false, true })
			{
				const auto fragment = motions ? compileRuntimeFragment(path, { "MOTIONS" }) : compileRuntimeFragment(path, {});
				SpvReflectShaderModule module{};
				Require(spvReflectCreateShaderModule(fragment.Num() * sizeof(uint32_t), fragment.GetData(), &module) == SPV_REFLECT_RESULT_SUCCESS,
					"MRT shader must expose a compiled fragment interface");
				bool hasMotionOutput = false;
				for (uint32_t i = 0u; i < module.entry_points[0].output_variable_count; ++i)
				{
					const auto* output = module.entry_points[0].output_variables[i];
					if (output->location == 1u)
					{
						hasMotionOutput = true;
						Require(output->numeric.vector.component_count == 4u, "velocity MRT must carry XY motion, depth and coverage");
					}
				}
				spvReflectDestroyShaderModule(&module);
				Require(hasMotionOutput == motions, "MOTIONS must control the second MRT output without a separate geometry pass");
			}
		}
		const auto skinnedMotion = compileRuntimeVertex("Shaders/Standard_glTF.shader", { "MOTIONS", "SKINNING", "TRANSMISSION" });
		RequireSpirvStorageBufferArrayStride(skinnedMotion, 0u, 2u, sizeof(glm::mat4));
		RequireSpirvStorageBufferArrayStride(skinnedMotion, 2u, 0u, sizeof(Framegraph::RenderSceneNode::PerInstanceData));
		compileRuntimeFragment("Shaders/Standard_glTF.shader", { "MOTIONS", "ALPHA_CUTOUT", "TRANSMISSION", "CLEAR_COAT", "SHEEN" });
		const auto motionBlur = compileRuntimeFragment("Shaders/MotionBlur.shader", {});
		RequireSpirvCombinedImageSamplerBinding(motionBlur, 1u, 3u);
		compileRuntimeFragment("Shaders/MotionBlur.shader", { "DEBUG_MOTIONS" });
		compileRuntimeVertex("Experimental/MeshParticles/Particle.shader", {});
		compileRuntimeFragment("Experimental/MeshParticles/Particle.shader", {});
		compileRuntimeVertex("Tests/Shaders/DepthCoverage.shader", {});
		compileRuntimeFragment("Tests/Shaders/DepthCoverage.shader", {});
		const RHI::ShaderByteCode hbaoByteCode = compileRuntimeFragment(
			"Shaders/HBAO.shader",
			{});
		RequireSpirvCombinedImageSamplerBinding(hbaoByteCode, 1u, 1u);
		RequireSpirvCombinedImageSamplerBinding(hbaoByteCode, 1u, 2u);
		compileRuntimeFragment("Shaders/HBAO_Blur.shader", { "VERTICAL" });
		compileRuntimeFragment("Shaders/HBAO_Blur.shader", { "HORIZONTAL" });
		const RHI::ShaderByteCode debugAoByteCode = compileRuntimeFragment(
			"Shaders/Debug.shader",
			{ "AO" });
		RequireSpirvCombinedImageSamplerBinding(
			debugAoByteCode,
			2u,
			8u);
		compileRuntimeFragment("Shaders/Debug.shader", { "CASCADES" });
		compileRuntimeFragment("Shaders/Debug.shader", { "LIGHT_TILES" });
		RequireSkyUniformLayout(compileRuntimeFragment("Shaders/Sky.shader", { "ATMOSPHERE" }));
		RequireSkyUniformLayout(compileRuntimeFragment("Shaders/Sky.shader", { "SUN" }));
		RequireSkyUniformLayout(compileRuntimeFragment("Shaders/Sky.shader", { "CLOUDS" }));
		RequireSkyUniformLayout(compileRuntimeFragment(
			"Shaders/Sky.shader",
			{ "CLOUDS", "DITHER" }));
		RequireSkyUniformLayout(compileRuntimeFragment("Shaders/SunShafts.shader", {}));
		compileRuntimeFragment("Shaders/Tonemapping.shader", { "AGX" });
		compileRuntimeFragment("Shaders/Tonemapping.shader", { "ACES" });
		compileRuntimeFragment(
			"Shaders/Tonemapping.shader",
			{ "ACES", "LUMINANCE" });
		compileRuntimeFragment(
			"Shaders/Tonemapping.shader",
			{ "UNCHARTED2" });

		auto compileRuntimeCompute = [](
			const char* computeShaderPath,
			std::initializer_list<const char*> permutationDefines = {}) -> RHI::ShaderByteCode
		{
			return CompileRuntimeShaderStage(computeShaderPath, permutationDefines, RHI::EShaderStage::Compute);
		};

		for (bool depthLayout : { false, true })
		{
			const auto culling = depthLayout ?
				compileRuntimeCompute("Shaders/ComputeMeshCulling.shader", { "OCCLUSION_CULLING", "DEPTH_INSTANCE_LAYOUT" }) :
				compileRuntimeCompute("Shaders/ComputeMeshCulling.shader", { "OCCLUSION_CULLING" });
			RequireSpirvStorageBufferArrayStride(culling, 1u, 0u,
				depthLayout ? sizeof(DepthPrepassNode::PerInstanceData) :
				sizeof(Framegraph::RenderSceneNode::PerInstanceData));
			RequireSpirvCombinedImageSamplerBinding(culling, 0u, 0u);
			SpvReflectShaderModule module{};
			Require(spvReflectCreateShaderModule(culling.Num() * sizeof(uint32_t),
				culling.GetData(), &module) == SPV_REFLECT_RESULT_SUCCESS, "culling SPIR-V must reflect");
			const bool valid = module.push_constant_block_count == 1u &&
				module.push_constant_blocks[0].size == sizeof(RHI::GpuCullingPushConstants) &&
				module.push_constant_blocks[0].member_count == 7u &&
				module.push_constant_blocks[0].members[5].offset == offsetof(RHI::GpuCullingPushConstants, m_phase) &&
				module.push_constant_blocks[0].members[6].offset == offsetof(RHI::GpuCullingPushConstants, m_bEnableOcclusion);
			spvReflectDestroyShaderModule(&module);
			Require(valid, "host and compute shader must agree on phase/occlusion push constants");
		}
		const auto depthInput = compileRuntimeCompute("Shaders/ComputeDepthHighZ.shader", { "DEPTH_INPUT" });
		RequireSpirvCombinedImageSamplerBinding(depthInput, 0u, 0u);
		RequireSpirvStorageImageBinding(depthInput, 0u, 1u);
		const auto depthMsaa = compileRuntimeCompute("Shaders/ComputeDepthHighZ.shader", { "MSAA_DEPTH_INPUT" });
		RequireSpirvCombinedImageSamplerBinding(depthMsaa, 0u, 0u);
		RequireSpirvStorageImageBinding(depthMsaa, 0u, 1u);
		SpvReflectShaderModule depthModule{};
		Require(spvReflectCreateShaderModule(depthMsaa.Num() * sizeof(uint32_t), depthMsaa.GetData(),
			&depthModule) == SPV_REFLECT_RESULT_SUCCESS, "MSAA depth input must reflect");
		SpvReflectResult depthStatus;
		const auto* depthBinding = spvReflectGetDescriptorBinding(&depthModule, 0u, 0u, &depthStatus);
		const bool multisampled = depthStatus == SPV_REFLECT_RESULT_SUCCESS && depthBinding &&
			depthBinding->image.ms == 1u && depthBinding->image.dim == SpvDim2D;
		spvReflectDestroyShaderModule(&depthModule);
		Require(multisampled, "Hi-Z input must retain access to each depth sample before reduction");
		const auto depthMips = compileRuntimeCompute("Shaders/ComputeDepthHighZ.shader");
		RequireSpirvStorageImageBinding(depthMips, 0u, 0u);
		RequireSpirvStorageImageBinding(depthMips, 0u, 1u);
		const RHI::ShaderByteCode lightCullingByteCode =
			compileRuntimeCompute("Shaders/ComputeLightCulling.shader");
		RequireSpirvStorageBufferArrayStride(
			lightCullingByteCode,
			0u,
			0u,
			sizeof(RHI::RHILightShaderData));
		compileRuntimeCompute("Shaders/ComputeHistogram.shader");
		compileRuntimeCompute("Shaders/ComputeAverageLuminance.shader");
		compileRuntimeCompute("Shaders/ComputeBloomDownscale.shader");
		compileRuntimeCompute("Shaders/ComputeBloomUpscale.shader");
		const RHI::ShaderByteCode brdfLutByteCode =
			compileRuntimeCompute("Shaders/ComputeBrdfLut.shader");
		RequireSpirvStorageImageBinding(brdfLutByteCode, 0u, 0u);
		const RHI::ShaderByteCode sheenEnvironmentByteCode =
			compileRuntimeCompute("Shaders/ComputeSheenEnvMap.shader");
		RequireSpirvCombinedImageSamplerBinding(
			sheenEnvironmentByteCode,
			0u,
			0u);
		RequireSpirvStorageImageBinding(
			sheenEnvironmentByteCode,
			0u,
			1u);
		const RHI::ShaderByteCode giResolveByteCode =
			compileRuntimeCompute("Shaders/GlobalIlluminationResolve.shader");
		for (uint32_t binding = 12u; binding <= 14u; ++binding)
		{
			RequireSpirvStorageBufferBinding(giResolveByteCode, 1u, binding);
		}
		RequireSpirvStorageBufferArrayStride(
			giResolveByteCode,
			1u,
			13u,
			sizeof(RHI::RHIGlobalIlluminationGpuBvhNode));
		RequireSpirvStorageBufferArrayStride(
			giResolveByteCode,
			1u,
			14u,
			sizeof(RHI::RHIGlobalIlluminationGpuBrick));
		for (uint32_t binding = 15u; binding <= 21u; ++binding)
		{
			RequireSpirvDescriptorBindingAbsent(
				giResolveByteCode,
				1u,
				binding);
		}
		RequireSpirvDescriptorBindingAbsent(giResolveByteCode, 1u, 3u);
		RequireSpirvCombinedImageSamplerBinding(giResolveByteCode, 2u, 0u);
		RequireSpirvStorageImageBinding(giResolveByteCode, 2u, 1u);
		RequireSpirvDescriptorBindingAbsent(giResolveByteCode, 2u, 2u);
		RequireSpirvDescriptorBindingAbsent(giResolveByteCode, 2u, 3u);
		RequireSpirvDescriptorBindingAbsent(giResolveByteCode, 2u, 4u);
	}

	void TestShadowCasterPermutationsCompile()
	{
		auto compilePermutation = [](
			const char* shaderPath,
			std::initializer_list<const char*> permutationDefines, bool hasPushConstants = true)
		{
			for (const bool debug : { false, true })
			{
				TVector<VulkanShaderStagePtr> stages;
				for (const auto stage : { RHI::EShaderStage::Vertex, RHI::EShaderStage::Fragment })
				{
					stages.Add(ReflectStage(CompileRuntimeShaderStage(shaderPath, permutationDefines, stage, debug), stage));
				}
				RequirePushConstantLayout(stages, VK_SHADER_STAGE_VERTEX_BIT, 0u, hasPushConstants ? 64u : 0u);
			}
		};

		compilePermutation("Shaders/ShadowCaster.shader", {});
		compilePermutation("Shaders/ShadowCaster.shader", { "EVSM" });
		compilePermutation("Shaders/ShadowCaster.shader", { "SKINNING" });
		compilePermutation("Shaders/ShadowCaster.shader", { "EVSM", "SKINNING" });
		compilePermutation("Shaders/ShadowCaster.shader", { "MASKED" });
		compilePermutation("Shaders/ShadowCaster.shader", { "EVSM", "MASKED" });
		compilePermutation("Shaders/ShadowCaster.shader", { "SKINNING", "MASKED" });
		compilePermutation("Shaders/ShadowCaster.shader", { "EVSM", "SKINNING", "MASKED" });
		compilePermutation("Experimental/MeshParticles/Particle.shader", { "SHADOW_CASTER" }, false);
		compilePermutation("Experimental/MeshParticles/Particle.shader", { "PACKED_SHADOW_CASTER" });
		compilePermutation("Experimental/MeshParticles/Particle.shader", { "PACKED_SHADOW_CASTER", "EVSM" });
		compilePermutation("Experimental/MeshParticles/Particle.shader", { "PACKED_SHADOW_CASTER", "SHADOW_CASTER" });
		compilePermutation("Experimental/MeshParticles/Particle.shader", { "PACKED_SHADOW_CASTER", "SHADOW_CASTER", "EVSM" });
	}

	void TestShaderSourceNormalization()
	{
		std::string diagnostic;
		std::string rawGlsl =
			"vec3 convertRGB2XYZ(vec3 value)\n"
			"{\n"
			"    // Reference(s):\n"
			"\treturn value;\n"
			"}\n";
		const std::string originalRawGlsl = rawGlsl;
		Require(
			!ShaderCompilerTestAccess::NormalizeShaderTabs("glsl", rawGlsl, diagnostic),
			"raw GLSL should not be normalized as shader YAML");
		Require(rawGlsl == originalRawGlsl && diagnostic.empty(),
			"raw GLSL should remain byte-for-byte unchanged without a YAML diagnostic");

		std::string validShader =
			"glslVertex: |\n"
			"  void main() {}\n";
		const std::string originalValidShader = validShader;
		Require(
			!ShaderCompilerTestAccess::NormalizeShaderTabs("shader", validShader, diagnostic),
			"valid shader YAML should not need normalization");
		Require(validShader == originalValidShader && diagnostic.empty(),
			"valid shader YAML should remain byte-for-byte unchanged");

		std::string shaderWithTab =
			"glslVertex: |\n"
			"  void main()\n"
			"  {\n"
			"\tgl_Position = vec4(0.0);\n"
			"  }\n";
		Require(
			ShaderCompilerTestAccess::NormalizeShaderTabs("shader", shaderWithTab, diagnostic),
			"invalid YAML indentation tabs should be normalized");
		Require(shaderWithTab.find('\t') == std::string::npos && diagnostic.empty(),
			"normalized shader YAML should contain no tabs or diagnostic");
		Require(shaderWithTab.find("\n    gl_Position") != std::string::npos,
			"shader YAML tabs should expand to exactly four spaces");
		Require(YAML::Load(shaderWithTab)["glslVertex"].IsScalar(),
			"normalized shader YAML should be parseable");

		std::string malformedShader = "glslVertex: [\n";
		const std::string originalMalformedShader = malformedShader;
		Require(
			!ShaderCompilerTestAccess::NormalizeShaderTabs("shader", malformedShader, diagnostic),
			"non-tab YAML errors should not trigger a rewrite");
		Require(malformedShader == originalMalformedShader && !diagnostic.empty(),
			"non-tab YAML errors should preserve the source and publish a diagnostic");
	}

	void TestShaderSourceRewritePreservesFileIdentity()
	{
		TempDirectory directory;
		const std::filesystem::path shaderPath = directory.Path("User.shader");
		const std::filesystem::path hardLinkPath = directory.Path("User.shader.link");
		const std::string onDiskSource =
			"glslVertex: |\r\n"
			"  void main()\n"
			"  {\r\n"
			"\tgl_Position = vec4(0.0);\n"
			"  }\r\n";
		{
			std::ofstream output(shaderPath, std::ios::binary);
			Require(output.is_open(), "the shader rewrite fixture should be writable");
			output << onDiskSource;
		}

		std::string originalSource;
		std::string diagnostic;
		Require(
			ShaderCompilerTestAccess::ReadShaderSourceBinary(
				shaderPath.generic_string(),
				originalSource,
				diagnostic),
			"the shader rewrite fixture should use production binary reading: " + diagnostic);
		Require(originalSource == onDiskSource,
			"production shader reading should preserve mixed line endings byte-for-byte");
		std::string normalizedSource = originalSource;
		Require(
			ShaderCompilerTestAccess::NormalizeShaderTabs(
				"shader",
				normalizedSource,
				diagnostic),
			"the rewrite fixture should require normalization");
		std::error_code filesystemError;
		std::filesystem::create_hard_link(shaderPath, hardLinkPath, filesystemError);
		Require(!filesystemError,
			"the shader rewrite fixture should support hard links: " + filesystemError.message());
		const auto permissionsBefore = std::filesystem::status(shaderPath).permissions();

		Require(
			ShaderCompilerTestAccess::RewriteShaderSourceInPlace(
				shaderPath.generic_string(),
				originalSource,
				normalizedSource,
				diagnostic),
			"normalized shader source should be written in place: " + diagnostic);
		Require(ReadText(shaderPath) == normalizedSource && ReadText(hardLinkPath) == normalizedSource,
			"in-place normalization should preserve the source inode");
		Require(std::filesystem::status(shaderPath).permissions() == permissionsBefore,
			"in-place normalization should preserve source permissions");
		Require(CountRegularFiles(shaderPath.parent_path()) == 2,
			"in-place normalization should not create discoverable temporary assets");

		const std::string onDiskConcurrentSource = "glslVertex: concurrent-edit\r\n";
		{
			std::ofstream output(shaderPath, std::ios::binary | std::ios::trunc);
			Require(output.is_open(), "the concurrent shader edit should be writable");
			output << onDiskConcurrentSource;
		}
		const std::string concurrentSource = onDiskConcurrentSource;
		Require(
			!ShaderCompilerTestAccess::RewriteShaderSourceInPlace(
				shaderPath.generic_string(),
				originalSource,
				normalizedSource,
				diagnostic),
			"a concurrent user edit should cancel shader normalization");
		Require(ReadText(shaderPath) == concurrentSource && !diagnostic.empty(),
			"a concurrent user edit should remain intact and produce a diagnostic");
	}
}

int main()
{
	try
	{
		TestArtifactRoundTrip();
		TestTruncatedArtifactIsRejected();
		TestMisalignedArtifactIsRejected();
		TestChecksumMismatchIsRejected();
		TestOwnedArtifactContainment();
		TestDebugArtifactsAreRequired();
		TestPayloadIgnoresUnknownFields();
		TestWarmPermutationReadsEachArtifactOnce();
		TestReloadKeepsHealthyShaderPermutations();
		TestPartialRecoveryRetriesFailedManifestCommit();
		TestFailedGenerationPreservesDurableGeneration();
		TestRemoveCommitsBeforeGarbageCollection();
		TestExplicitInvalidationSurvivesSameTimestampReload();
		TestMissingStorageRecoveryDropsStaleArtifactReferences();
		TestSameSizeChecksumCorruptionAndClearExpiredTransaction();
		TestIoFailureQuarantineIsReadOnlyAndSessionOnly();
		TestRuntimeArtifactIoFailureEntersReadOnlyQuarantine();
		TestShaderCompilerFailureLifecycle();
		TestFailedGlslCompilationPreservesBytecode();
		TestShaderDependencyFingerprintTracksTimestampAndWinner();
		TestMissingYamlIncludeFailsWithoutPartialSource();
		TestCompiledPushConstantRanges();
		TestRuntimePushConstantLayouts();
		TestRuntimeLightingShadersCompile();
		TestShadowCasterPermutationsCompile();
		TestShaderSourceNormalization();
		TestShaderSourceRewritePreservesFileIdentity();
		std::cout << "Shader cache artifact tests passed.\n";
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "Shader cache artifact tests failed: " << exception.what() << '\n';
		return 1;
	}
}
