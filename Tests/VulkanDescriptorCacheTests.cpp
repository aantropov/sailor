#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "GraphicsDriver/Vulkan/VulkanPipeline.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "GraphicsDriver/Vulkan/VulkanShaderModule.h"
#include "FrameGraph/RenderSceneTextureCache.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "RHI/Material.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <cctype>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	class VulkanGraphicsDriverProbe final : public VulkanGraphicsDriver
	{
	public:
		using DescriptorCacheKey = CachedDescriptorSet;
		using ComputeCacheKey = ComputePipelineCacheKey;
	};

	class VulkanDescriptorSetOwnershipProbe final : public VulkanDescriptorSet
	{
	public:
		using DescriptorPoolMemberType = decltype(m_descriptorPool);
		using DescriptorPoolPageMemberType = decltype(m_descriptorPoolPage);
	};

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(), "test source should be readable: " + path.generic_string());
		return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}

	std::string ExtractFunctionBody(const std::string& source, const std::string& signature)
	{
		const size_t signatureOffset = source.find(signature);
		Require(signatureOffset != std::string::npos,
			"function signature should exist: " + signature);

		const size_t bodyOffset = source.find('{', signatureOffset + signature.size());
		Require(bodyOffset != std::string::npos,
			"function body should exist: " + signature);

		size_t depth = 0;
		for (size_t offset = bodyOffset; offset < source.size(); ++offset)
		{
			if (source[offset] == '{')
			{
				++depth;
			}
			else if (source[offset] == '}' && --depth == 0)
			{
				return source.substr(bodyOffset, offset - bodyOffset + 1);
			}
		}

		throw std::runtime_error("function body should be balanced: " + signature);
	}

	std::string RemoveWhitespace(const std::string& value)
	{
		std::string result;
		result.reserve(value.size());
		for (const unsigned char character : value)
		{
			if (!std::isspace(character))
			{
				result.push_back(static_cast<char>(character));
			}
		}
		return result;
	}

	RHI::RHITexturePtr MakeTexture()
	{
		return RHI::RHITexturePtr::Make(
			RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping::Repeat,
			false);
	}

	void SetTextureCount(
		RHI::RHIShaderBindingSetPtr bindings,
		uint32_t textureCount)
	{
		auto& textureBinding =
			bindings->GetOrAddShaderBinding("textureSamplers");
		TVector<RHI::RHITexturePtr> textures;
		textures.Reserve(textureCount);
		for (uint32_t i = 0; i < textureCount; ++i)
		{
			textures.Add(MakeTexture());
		}

		textureBinding->SetTextureBindings(textures);
		bindings->RecalculateCompatibility();
	}

	void TestSsboElementAlignmentPreservesStd430Stride()
	{
		Require(SsboLayout::AlignSsboElementSize(112u) == 112u,
			"an already aligned PerInstanceData stride must not gain a phantom 16-byte gap");
		Require(SsboLayout::AlignSsboElementSize(96u) == 96u,
			"legacy aligned std430 strides must remain unchanged");
		Require(SsboLayout::AlignSsboElementSize(100u) == 112u,
			"an unaligned SSBO element size must round up to the next 16-byte boundary");

		SpvReflectTypeDescription reflectedType{};
		SpvReflectBlockVariable reflectedArray{};
		reflectedArray.type_description = &reflectedType;

		reflectedArray.array.stride = 112u;
		reflectedType.traits.array.stride = 144u;
		reflectedArray.padded_size = 132u;
		Require(SsboLayout::ResolveSsboArrayStride(reflectedArray) == 112u,
			"SPIR-V runtime-array stride must remain the authoritative SSBO instance stride");

		reflectedArray.array.stride = 0u;
		reflectedType.traits.array.stride = 112u;
		reflectedArray.padded_size = 112u;
		Require(SsboLayout::ResolveSsboArrayStride(reflectedArray) == 112u,
			"PerInstanceData must use its reflected type-array stride");

		reflectedType.traits.array.stride = 144u;
		reflectedArray.padded_size = 132u;
		Require(SsboLayout::ResolveSsboArrayStride(reflectedArray) == 144u,
			"MaterialData must use its reflected type-array stride instead of its unpadded member size");

		reflectedType.traits.array.stride = 0u;
		reflectedArray.padded_size = 112u;
		Require(SsboLayout::ResolveSsboArrayStride(reflectedArray) == 112u,
			"non-array storage blocks must fall back to their reflected padded size");
	}

	void TestDescriptorCacheKeyKeepsItsCompatibilitySnapshot()
	{
		using DescriptorCacheKey =
			VulkanGraphicsDriverProbe::DescriptorCacheKey;

		VulkanPipelineLayoutPtr layout = VulkanPipelineLayoutPtr::Make();
		RHI::RHIShaderBindingSetPtr bindings =
			RHI::RHIShaderBindingSetPtr::Make();
		SetTextureCount(bindings, 1);

		DescriptorCacheKey oldKey(layout, bindings);
		const DescriptorCacheKey oldKeyCopy = oldKey;
		const size_t oldHash = oldKey.GetHash();

		TConcurrentMap<DescriptorCacheKey, uint32_t> cache;
		cache.At_Lock(oldKey) = 7u;
		cache.Unlock(oldKey);
		Require(cache.Num() == 1,
			"the descriptor cache fixture must contain the original key");

		SetTextureCount(bindings, 2);
		Require(oldKey.IsExpired(),
			"a key must expire when its binding compatibility changes");
		Require(oldKey.GetHash() == oldHash &&
			oldKeyCopy.GetHash() == oldHash,
			"an expired key must retain its original hash");
		Require(oldKey == oldKeyCopy,
			"copies of an immutable key must remain equal after the binding changes");

		DescriptorCacheKey assignedKey;
		assignedKey = oldKey;
		Require(assignedKey.GetHash() == oldHash && assignedKey == oldKey,
			"copy assignment must preserve the captured compatibility value");

		const DescriptorCacheKey currentKey(layout, bindings);
		Require(!(currentKey == oldKey) && !(oldKey == currentKey),
			"keys captured before and after a compatibility change must differ symmetrically");

		Require(cache.Remove(oldKeyCopy),
			"an expired descriptor cache entry must remain removable by its immutable key");
		Require(cache.IsEmpty(),
			"removing the expired descriptor cache key must release its map entry");
	}

	void TestDescriptorCacheKeyTracksDescriptorRevision()
	{
		using DescriptorCacheKey =
			VulkanGraphicsDriverProbe::DescriptorCacheKey;

		VulkanPipelineLayoutPtr layout = VulkanPipelineLayoutPtr::Make();
		RHI::RHIShaderBindingSetPtr bindings =
			RHI::RHIShaderBindingSetPtr::Make();
		SetTextureCount(bindings, 2);
		bindings->AdvanceDescriptorRevision();

		const DescriptorCacheKey oldKey(layout, bindings);
		const DescriptorCacheKey oldKeyCopy = oldKey;
		const size_t oldHash = oldKey.GetHash();

		bindings->AdvanceDescriptorRevision();
		Require(oldKey.IsExpired(),
			"a key must expire when descriptor contents get a new revision");
		Require(oldKey.GetHash() == oldHash && oldKeyCopy.GetHash() == oldHash,
			"an expired key must retain its captured descriptor revision");

		const DescriptorCacheKey currentKey(layout, bindings);
		Require(!(currentKey == oldKey) && !(oldKey == currentKey),
			"keys from different descriptor revisions must differ symmetrically");
	}

	void TestTextureSamplerUpdatesUseSynchronizedSnapshot()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string importerHeader = ReadText(
			sourceRoot / "Runtime/AssetRegistry/Texture/TextureImporter.h");
		const std::string importerSource = ReadText(
			sourceRoot / "Runtime/AssetRegistry/Texture/TextureImporter.cpp");
		const std::string snapshotBody = ExtractFunctionBody(
			importerSource,
			"TextureImporter::TextureSamplersSnapshot TextureImporter::GetTextureSamplersSnapshot(");
		const std::string updateBody = ExtractFunctionBody(
			importerSource,
			"bool TextureImporter::UpdateTextureSamplerBinding(");
		const std::string hotReloadBody = ExtractFunctionBody(
			importerSource,
			"void TextureImporter::OnUpdateAssetInfo(");
		const std::string loadBody = ExtractFunctionBody(
			importerSource,
			"Tasks::TaskPtr<TexturePtr> TextureImporter::LoadTexture(");

		Require(importerHeader.find("TextureSamplersSnapshot") != std::string::npos &&
			importerHeader.find("mutable SpinLock m_textureSamplersLock") != std::string::npos,
			"texture sampler contents and descriptor revision must be captured under one importer spin lock");
		Require(snapshotBody.find("m_textureSamplersLock") != std::string::npos &&
			updateBody.find("m_textureSamplersLock") != std::string::npos,
			"texture sampler reads and writes must share the importer spin lock");
		Require(hotReloadBody.find("UpdateTextureSamplerBinding(") != std::string::npos &&
			loadBody.find("RegisterTextureSamplerBinding(") != std::string::npos,
			"all global texture sampler writers must use the synchronized update path");
	}

	void TestGeneratedGltfTexturesReuseTheirExistingAssetIds()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string importerSource = ReadText(
			sourceRoot / "Runtime/AssetRegistry/Texture/TextureImporter.cpp");
		const std::string extractBody = ExtractFunctionBody(
			importerSource,
			"bool ExtractTextureFromGltf(");
		const std::string importBody = ExtractFunctionBody(
			importerSource,
			"bool TextureImporter::ImportTexture(");

		Require(extractBody.find("loader.SetImagesAsIs(true)") != std::string::npos &&
			extractBody.find("loader.LoadASCIIFromFile(") != std::string::npos &&
			extractBody.find("gltfModel.textures") != std::string::npos &&
			extractBody.find("ResolveGltfTextureImageIndex(") != std::string::npos &&
			extractBody.find("gltfModel.images") != std::string::npos &&
			extractBody.find("image.image") != std::string::npos,
			"generated glTF textures must extract encoded data URI, bufferView, or external image bytes through tinygltf");
		Require(importBody.find("const bool bIsGltf = extension == \"gltf\"") != std::string::npos &&
			importBody.find("ExtractTextureFromGltf(") != std::string::npos &&
			importBody.find("ExtractTextureFromGLB(") != std::string::npos,
			"texture import must support generated textures from glTF while retaining the established GLB path");
		Require(extractBody.find("FileId") == std::string::npos &&
			extractBody.find("CreateTextureAsset") == std::string::npos &&
			extractBody.find("AssetRegistry") == std::string::npos,
			"extracting an existing secondary texture must not allocate or replace its asset identity");
	}

	void TestVariableDescriptorCountCollectionUsesPublishedSnapshots()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string driverSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanGraphicsDriver.cpp");
		const std::string collectBody = ExtractFunctionBody(
			driverSource,
			"TVector<uint32_t> VulkanGraphicsDriver::CollectOptionalVariableDescriptorCount(");
		const std::string publishedCollectBody = ExtractFunctionBody(
			driverSource,
			"TVector<uint32_t> VulkanGraphicsDriver::CollectPublishedVariableDescriptorCounts(");
		const std::string dispatchBody = ExtractFunctionBody(
			driverSource,
			"void VulkanGraphicsDriver::Dispatch(");
		const std::string pipelineBody = ExtractFunctionBody(
			driverSource,
			"VulkanComputePipelinePtr VulkanGraphicsDriver::GetOrAddComputePipeline(");

		Require(collectBody.find("m_descriptorUpdateMutex") == std::string::npos &&
			collectBody.find("GetLayoutBindings()") == std::string::npos &&
			collectBody.find("GetShaderBindings()") != std::string::npos &&
			collectBody.find("reflectedBinding.m_name") != std::string::npos,
			"variable descriptor counts must use immutable owned bindings without the global descriptor mutex");
		Require(publishedCollectBody.find("m_descriptorUpdateMutex") == std::string::npos &&
			publishedCollectBody.find("GetVariableDescriptorCount()") != std::string::npos &&
			publishedCollectBody.find("GetLayoutBindings()") == std::string::npos &&
			publishedCollectBody.find("GetShaderBindings()") == std::string::npos,
			"dispatch must read only the published per-set descriptor capacities");
		Require(dispatchBody.find("CollectPublishedVariableDescriptorCounts(bindings)") != std::string::npos &&
			pipelineBody.find("CollectOptionalVariableDescriptorCount(") == std::string::npos &&
			pipelineBody.find("ComputePipelineCacheKey") != std::string::npos,
			"dispatch must use lock-free per-set counts and include them in the compute-pipeline cache key");

		using ComputeCacheKey = VulkanGraphicsDriverProbe::ComputeCacheKey;
		RHI::RHIShaderPtr shader = RHI::RHIShaderPtr::Make(RHI::EShaderStage::Compute);
		const TVector<uint32_t> counts{ 0u, 8192u };
		const TVector<uint32_t> otherCounts{ 0u, 1024u };
		const ComputeCacheKey eightBytePushConstants(shader, 8u, &counts);
		const ComputeCacheKey sixteenBytePushConstants(shader, 16u, &counts);
		const ComputeCacheKey differentDescriptorCount(shader, 8u, &otherCounts);
		Require(eightBytePushConstants == sixteenBytePushConstants &&
			eightBytePushConstants.GetHash() == sixteenBytePushConstants.GetHash(),
			"equivalent 256-byte Vulkan push-constant ranges must share one compute pipeline");
		Require(!(eightBytePushConstants == differentDescriptorCount),
			"different variable descriptor capacities must use different compute pipelines");
	}

	void TestRenderSceneTextureCacheTracksRequestedSlotRevisions()
	{
		const TVector<uint64_t> cachedSlotRevisions{ 11u, 29u, 47u };
		const TVector<uint64_t> unchangedSlotRevisions = cachedSlotRevisions;
		const TVector<uint64_t> changedSlotRevisions{ 11u, 30u, 47u };
		const TVector<uint64_t> missingSlotRevision{ 11u, 29u };

		Require(Framegraph::Details::CanReuseRenderSceneTextureBindings(1, 1, true),
			"an unchanged source revision must be a direct texture cache hit");
		Require(!Framegraph::Details::CanReuseRenderSceneTextureBindings(1, 1, false),
			"a matching revision must not manufacture a missing cached binding");
		Require(!Framegraph::Details::CanReuseRenderSceneTextureBindings(1, 2, true),
			"a newer source revision without slot snapshots must require revalidation");
		Require(Framegraph::Details::CanReuseRenderSceneTextureBindings(
			1, 2, true, &cachedSlotRevisions, &unchangedSlotRevisions),
			"an unrelated global update must reuse bindings whose requested slots are unchanged");
		Require(!Framegraph::Details::CanReuseRenderSceneTextureBindings(
			1, 2, true, &cachedSlotRevisions, &changedSlotRevisions),
			"a changed requested slot must invalidate the local descriptor set");
		Require(!Framegraph::Details::CanReuseRenderSceneTextureBindings(
			1, 2, true, &cachedSlotRevisions, &missingSlotRevision),
			"a missing requested-slot revision must invalidate the local descriptor set");
		Require(!Framegraph::Details::CanReuseRenderSceneTextureBindings(
			1, 2, true, nullptr, &unchangedSlotRevisions),
			"both requested-slot snapshots are required for revision revalidation");
	}

	void TestRenderSceneTextureBindingsUseDenseLocalIndices()
	{
		const TVector<uint32_t> globalTextureIndices{
			0u,
			7u,
			42u,
			383u,
			42u };
		const TVector<uint32_t> remap =
			Framegraph::Details::BuildDenseTextureRemap(
				globalTextureIndices);

		Require(remap.Num() == 384u,
			"the remap must address the largest global texture index");
		Require(remap[0] == 0u &&
			remap[7] == 1u &&
			remap[42] == 2u &&
			remap[383] == 3u,
			"requested global texture indices must map to deterministic dense slots");
		Require(remap[1] == 0u && remap[382] == 0u,
			"unrequested global texture slots must resolve to the default local texture");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string renderSceneSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/RenderSceneNode.cpp");
		const std::string depthPrepassSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/DepthPrepassNode.cpp");
		const std::string gltfShader = ReadText(
			sourceRoot / "Content/Shaders/Standard_glTF.shader");
		const std::string shaderCompilerSource = ReadText(
			sourceRoot /
			"Runtime/AssetRegistry/Shader/ShaderCompiler.cpp");
		const std::string vulkanDriverSource = ReadText(
			sourceRoot /
			"Runtime/GraphicsDriver/Vulkan/VulkanGraphicsDriver.cpp");
		const std::string compatibleDescriptorSetsBody = ExtractFunctionBody(
			vulkanDriverSource,
			"TVector<VulkanDescriptorSetPtr> VulkanGraphicsDriver::GetCompatibleDescriptorSets(");

		Require(renderSceneSource.find(
			"BuildDenseTextureRemap(key.m_requestedTextures)") !=
			std::string::npos &&
			renderSceneSource.find(
				"\"textureSamplerRemap\",") !=
				std::string::npos,
			"render batches must bind a dense global-to-local texture remap");
		Require(depthPrepassSource.find(
			"batch.m_textureBindings = Framegraph::Details::GetTextureBindingSet(") !=
			std::string::npos &&
			depthPrepassSource.find(
				"material->GetBindings(), batch.m_textureBindings") !=
				std::string::npos &&
			depthPrepassSource.find(
				"material->GetBindings(), textureSamplers") ==
				std::string::npos,
			"custom depth batches must use their dense texture bindings instead of the legacy global set");
		Require(gltfShader.find(
			"ResolveTextureSamplerIndex(material.baseColorSampler)") !=
			std::string::npos &&
			gltfShader.find(
				"layout(set=4, binding=1) uniform sampler2D textureSamplers[];") !=
				std::string::npos,
			"glTF shaders must resolve global sampler ids before indexing the dense descriptor array");
		Require(shaderCompilerSource.find(
			"vertexDefines.Add(\"SAILOR_TEXTURE_REMAP\")") !=
			std::string::npos &&
			shaderCompilerSource.find(
				"fragmentDefines.Add(\"SAILOR_TEXTURE_REMAP\")") !=
				std::string::npos,
			"Apple shader permutations must use the dense texture-remap ABI");
		Require(vulkanDriverSource.find(
			"return binding.m_set != MaterialDescriptorSet;") !=
			std::string::npos &&
			vulkanDriverSource.find(
				"constexpr uint32_t MaterialDescriptorSet = 3u;") !=
				std::string::npos,
			"material bindings must exclude the pass-local texture remap buffer from descriptor set 4");
		Require(renderSceneSource.find(
			"localTextures.Resize(MaxTextureSlotsPerBatch)") ==
				std::string::npos &&
			compatibleDescriptorSetsBody.find(
				"emittedTextureSlots = matchedLayoutBinding.descriptorCount;") !=
				std::string::npos,
			"Apple batch sets must write only dense textures while allocating the complete Metal argument-buffer array");
	}

	void TestTextureSamplerCapacityReservesDefaultSlot()
	{
		Require(TextureImporter::MaxTexturesInScene == 8192,
			"the bindless texture array must expose 8192 total slots");
		Require(TextureImporter::MaxUserTexturesInScene == 8191,
			"slot zero must leave exactly 8191 slots for imported textures");
		Require(!TextureImporter::IsUserTextureSamplerIndexValid(0),
			"slot zero must remain reserved for the default texture");
		Require(TextureImporter::IsUserTextureSamplerIndexValid(1) &&
			TextureImporter::IsUserTextureSamplerIndexValid(8191),
			"all user texture slots from one through 8191 must be valid");
		Require(!TextureImporter::IsUserTextureSamplerIndexValid(8192),
			"the first index past the full descriptor array must be rejected");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string importerSource = ReadText(
			sourceRoot / "Runtime/AssetRegistry/Texture/TextureImporter.cpp");
		const std::string registerBindingBody = ExtractFunctionBody(
			importerSource,
			"bool TextureImporter::RegisterTextureSamplerBinding(");
		const size_t updateOffset = registerBindingBody.find("UpdateTextureSamplerBindingLocked(");
		const size_t publishOffset = registerBindingBody.find("m_textureSamplersCurrentIndex.store(");
		Require(registerBindingBody.find("IsUserTextureSamplerIndexValid(") != std::string::npos,
			"the runtime index allocator must enforce the public 8191-slot boundary contract");
		Require(updateOffset != std::string::npos &&
			publishOffset != std::string::npos &&
			updateOffset < publishOffset,
			"a texture sampler index must be published only after its descriptor write succeeds");
	}

	void TestGlobalTextureSamplerUsesFixedCapacityInPlaceUpdates()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string driverSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanGraphicsDriver.cpp");
		const std::string descriptorsHeader = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanDescriptors.h");
		const std::string descriptorsSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanDescriptors.cpp");
		const std::string updateSetBody = ExtractFunctionBody(
			driverSource,
			"bool VulkanGraphicsDriver::UpdateDescriptorSet(");
		const std::string updateBindingBody = ExtractFunctionBody(
			driverSource,
			"void VulkanGraphicsDriver::UpdateShaderBinding(");
		const std::string updateDescriptorBody = ExtractFunctionBody(
			descriptorsSource,
			"bool VulkanDescriptorSet::UpdateDescriptor(");
		const std::string referencesImageViewBody = ExtractFunctionBody(
			descriptorsSource,
			"bool VulkanDescriptorSet::ReferencesImageView(");
		const std::string normalizedUpdateSetBody = RemoveWhitespace(updateSetBody);
		const std::string normalizedUpdateDescriptorBody = RemoveWhitespace(updateDescriptorBody);

		Require(updateSetBody.find("plannedTextureSlots") != std::string::npos &&
			updateSetBody.find("variableDescriptorCount") != std::string::npos &&
			(normalizedUpdateSetBody.find("variableDescriptorCount=std::max(variableDescriptorCount,plannedTextureSlots)") != std::string::npos ||
				normalizedUpdateSetBody.find("variableDescriptorCount=(std::max)(variableDescriptorCount,plannedTextureSlots)") != std::string::npos ||
				normalizedUpdateSetBody.find("variableDescriptorCount=plannedTextureSlots") != std::string::npos),
			"the first bindless set allocation must reserve the full declared texture capacity");
		Require(descriptorsHeader.find("UpdateDescriptor(VulkanDescriptorPtr descriptor)") != std::string::npos,
			"descriptor sets must expose a single-descriptor in-place update operation");
		Require(updateBindingBody.find("IsDescriptorUpdateAfterBindSupported()") != std::string::npos &&
			updateBindingBody.find("bIsUnusedDescriptorSlot") != std::string::npos &&
			updateBindingBody.find("m_bVariableDescriptorCount") != std::string::npos &&
			updateBindingBody.find("UpdateDescriptor(") != std::string::npos &&
			updateBindingBody.find("AdvanceDescriptorRevision()") != std::string::npos &&
			updateBindingBody.find("const auto previousTextures = currentTextures") != std::string::npos &&
			updateBindingBody.find("SetTextureBindings(previousTextures)") != std::string::npos,
			"unused variable sampler slots must use update-after-bind and advance the source revision");
		Require(updateBindingBody.find("dstArrayElement >= layout.m_arrayCount") != std::string::npos,
			"variable descriptor writes past the immutable allocation must be rejected before CPU state changes");
		Require(updateDescriptorBody.find("descriptor->Apply(") != std::string::npos &&
			normalizedUpdateDescriptorBody.find("vkUpdateDescriptorSets(*m_device,1,") != std::string::npos &&
			updateDescriptorBody.find("UPDATE_UNUSED_WHILE_PENDING") != std::string::npos &&
			updateDescriptorBody.find("RecalculateCompatibility()") == std::string::npos &&
			updateBindingBody.find("RecalculateCompatibility()") == std::string::npos &&
			descriptorsHeader.find("m_retiredDescriptors") == std::string::npos,
			"an in-place texture update must write exactly one unused slot without rescanning compatibility or retaining replaced resources");

		const size_t directDescriptorLookup = referencesImageViewBody.find(
			"arrayElement < m_descriptors.Num()");
		const size_t linearDescriptorFallback = referencesImageViewBody.find(
			"for (const VulkanDescriptorPtr& descriptor : m_descriptors)");
		Require(directDescriptorLookup != std::string::npos &&
			linearDescriptorFallback != std::string::npos &&
			directDescriptorLookup < linearDescriptorFallback,
			"bindless image-view lookup must use its array element before the general linear fallback");
	}

	void TestVariableDescriptorCompatibilityUsesItsFixedLayout()
	{
		const VkDescriptorSetLayoutBinding textureLayout =
			VulkanApi::CreateDescriptorSetLayoutBinding(
				0,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				static_cast<uint32_t>(TextureImporter::MaxTexturesInScene));

		VulkanDescriptorSetLayoutPtr descriptorLayout =
			VulkanDescriptorSetLayoutPtr::Make(
				VulkanDevicePtr{},
				TVector<VkDescriptorSetLayoutBinding>{ textureLayout },
				0);
		VulkanDescriptorSetLayoutPtr pipelineDescriptorLayout =
			VulkanDescriptorSetLayoutPtr::Make(
				VulkanDevicePtr{},
				TVector<VkDescriptorSetLayoutBinding>{ textureLayout },
				0);
		VulkanDescriptorSetPtr descriptorSet = VulkanDescriptorSetPtr::Make(
			VulkanDevicePtr{},
			VulkanDescriptorPoolPtr{},
			descriptorLayout,
			TVector<VulkanDescriptorPtr>{},
			static_cast<uint32_t>(TextureImporter::MaxTexturesInScene));
		VulkanPipelineLayoutPtr pipelineLayout = VulkanPipelineLayoutPtr::Make();
		pipelineLayout->m_descriptionSetLayouts.Add(pipelineDescriptorLayout);

		Require(VulkanApi::IsCompatible(pipelineLayout, descriptorSet, 0),
			"a fixed variable descriptor layout must be compatible without scanning populated texture slots");
	}

	void TestVariableDescriptorCompatibilityRejectsDifferentLayoutCapacity()
	{
		constexpr uint32_t LocalTextureCapacity = 4u;
		const VkDescriptorSetLayoutBinding localTextureLayout =
			VulkanApi::CreateDescriptorSetLayoutBinding(
				0,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				LocalTextureCapacity);
		const VkDescriptorSetLayoutBinding pipelineTextureLayout =
			VulkanApi::CreateDescriptorSetLayoutBinding(
				0,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				static_cast<uint32_t>(TextureImporter::MaxTexturesInScene));

		VulkanDescriptorSetLayoutPtr descriptorLayout =
			VulkanDescriptorSetLayoutPtr::Make(
				VulkanDevicePtr{},
				TVector<VkDescriptorSetLayoutBinding>{ localTextureLayout },
				0);
		VulkanDescriptorSetLayoutPtr pipelineDescriptorLayout =
			VulkanDescriptorSetLayoutPtr::Make(
				VulkanDevicePtr{},
				TVector<VkDescriptorSetLayoutBinding>{ pipelineTextureLayout },
				0);

		TVector<VulkanDescriptorPtr> localDescriptors;
		for (uint32_t index = 0; index < LocalTextureCapacity; ++index)
		{
			localDescriptors.Add(VulkanDescriptorPtr::Make(
				0,
				index,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
		}

		VulkanDescriptorSetPtr descriptorSet = VulkanDescriptorSetPtr::Make(
			VulkanDevicePtr{},
			VulkanDescriptorPoolPtr{},
			descriptorLayout,
			std::move(localDescriptors),
			LocalTextureCapacity);
		VulkanPipelineLayoutPtr pipelineLayout = VulkanPipelineLayoutPtr::Make();
		pipelineLayout->m_descriptionSetLayouts.Add(pipelineDescriptorLayout);

		Require(!VulkanApi::IsCompatible(pipelineLayout, descriptorSet, 0),
			"a populated variable descriptor set must not bind against a pipeline layout with a different descriptorCount");
	}

	void TestShaderReflectionUsesDeclaredMemberOffsets()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string shaderModuleSource = ReadText(
			sourceRoot /
			"Runtime/GraphicsDriver/Vulkan/VulkanShaderModule.cpp");
		const std::string reflectionBody = RemoveWhitespace(
			ExtractFunctionBody(
				shaderModuleSource,
				"void VulkanShaderStage::ReflectDescriptorSetBindings("));

		Require(reflectionBody.find(
			"member.m_absoluteOffset=blockContent[i].offset;") !=
			std::string::npos,
			"named uniform writes must use SPIR-V member offsets instead of repacking aligned fields");
		Require(reflectionBody.find(
			"member.m_absoluteOffset=membersSize") ==
			std::string::npos,
			"shader reflection must not infer offsets by summing member sizes");
		Require(reflectionBody.find(
			"SsboLayout::ResolveSsboArrayStride(reflectedArray)") !=
			std::string::npos,
			"SSBO reflection must use the SPIR-V runtime-array stride without adding phantom padding");
	}

	void TestMaterialUniformUploadInitializesCompleteReflectedBlocks()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string materialSource = ReadText(
			sourceRoot /
			"Runtime/AssetRegistry/Material/MaterialImporter.cpp");
		const std::string updateBody = RemoveWhitespace(
			ExtractFunctionBody(
				materialSource,
				"void Material::UpdateUniforms("));

		const size_t initializeBlock = updateBody.find(
			"data.Resize(bindingSize);");
		const size_t writeMember = updateBody.find(
			"std::memcpy(data.GetData()+memberLayout.m_absoluteOffset,");
		const size_t uploadBlock = updateBody.find(
			"UpdateShaderBinding(cmdList,binding,data.m_second->GetData(),data.m_second->Num())");

		Require(initializeBlock != std::string::npos &&
			writeMember != std::string::npos &&
			uploadBlock != std::string::npos &&
			initializeBlock < writeMember &&
			writeMember < uploadBlock,
			"material uploads must zero complete reflected blocks before packing present values into one upload");
		Require(updateBody.find("UpdateShaderBindingVariable(") ==
			std::string::npos,
			"material fields must not be uploaded separately after the zero initialization");
	}

	void TestMaterialLoadPromiseCoversRhiInitialization()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string materialSource = ReadText(
			sourceRoot /
			"Runtime/AssetRegistry/Material/MaterialImporter.cpp");
		const std::string loadBody = RemoveWhitespace(
			ExtractFunctionBody(
				materialSource,
				"Tasks::TaskPtr<MaterialPtr> MaterialImporter::LoadMaterial("));

		const size_t createPromise = loadBody.find(
			"promise=Tasks::CreateTaskWithResult<MaterialPtr>(");
		const size_t updateRhi = loadBody.find(
			"pMaterial->UpdateRHIResource();",
			createPromise);
		const size_t updateUniforms = loadBody.find(
			"pMaterial->ForcelyUpdateUniforms();",
			updateRhi);
		const size_t returnMaterial = loadBody.find(
			"returnpMaterial;",
			updateUniforms);
		const size_t rhiThread = loadBody.find(
			"},EThreadType::RHI);",
			returnMaterial);
		const size_t loadTexture = loadBody.find(
			"LoadTexture(",
			rhiThread);
		const size_t joinSampler = loadBody.find(
			"promise->Join(updateSampler);",
			loadTexture);
		const size_t joinShader = loadBody.find(
			"promise->Join(pLoadShader);",
			joinSampler);
		const size_t runPromise = loadBody.find(
			"promise->Run();",
			joinShader);

		Require(createPromise != std::string::npos &&
			updateRhi != std::string::npos &&
			updateUniforms != std::string::npos &&
			returnMaterial != std::string::npos &&
			rhiThread != std::string::npos &&
			createPromise < updateRhi &&
			updateRhi < updateUniforms &&
			updateUniforms < returnMaterial &&
			returnMaterial < rhiThread,
			"the returned material promise must own the final RHI initialization");
		Require(loadTexture != std::string::npos &&
			joinSampler != std::string::npos &&
			joinShader != std::string::npos &&
			runPromise != std::string::npos &&
			loadTexture < joinSampler &&
			joinSampler < joinShader &&
			joinShader < runPromise,
			"the material promise must wait for texture and shader dependencies before it runs");
		Require(loadBody.find("updateRHI->Run()") == std::string::npos,
			"material loading must not return before a nested RHI task completes");
	}

	void TestDescriptorPoolsUseSmartOwnership()
	{
		Require((std::is_same_v<decltype(ThreadContext::m_descriptorPool), VulkanDescriptorPoolPtr>),
			"each Vulkan thread context must retain its descriptor pool through a smart pointer");
		Require((std::is_same_v<VulkanDescriptorSetOwnershipProbe::DescriptorPoolMemberType, VulkanDescriptorPoolPtr>),
			"descriptor allocation must receive smart descriptor-pool ownership");
		Require((std::is_same_v<VulkanDescriptorSetOwnershipProbe::DescriptorPoolPageMemberType, VulkanDescriptorPoolPagePtr>),
			"a descriptor set must retain its exact native pool page through a smart pointer");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string descriptorsSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanDescriptors.cpp");
		const std::string compileBody = ExtractFunctionBody(
			descriptorsSource,
			"bool VulkanDescriptorSet::TryCompile(");
		const std::string releaseBody = ExtractFunctionBody(
			descriptorsSource,
			"void VulkanDescriptorSet::Release(");
		const std::string poolPageDestructorBody = ExtractFunctionBody(
			descriptorsSource,
			"VulkanDescriptorPoolPage::~VulkanDescriptorPoolPage(");

		Require(compileBody.find("m_descriptorPool->AllocateDescriptorSet(") != std::string::npos &&
			compileBody.find("m_descriptorPoolPage") != std::string::npos &&
			compileBody.find("m_descriptorPool.Clear()") != std::string::npos,
			"descriptor sets must keep their allocated pool page while releasing the thread pool manager");
		Require(releaseBody.find("vkFreeDescriptorSets") == std::string::npos,
			"individual descriptor sets must not directly free page-owned native handles");
		Require(releaseBody.find("m_descriptorPoolPage.Clear()") != std::string::npos,
			"descriptor-set release must relinquish its smart pool-page ownership");
		Require(poolPageDestructorBody.find("vkDestroyDescriptorPool") != std::string::npos,
			"the last smart reference to a pool page must destroy its native pool");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "SsboElementAlignmentPreservesStd430Stride",
			TestSsboElementAlignmentPreservesStd430Stride },
		{ "DescriptorCacheKeyKeepsItsCompatibilitySnapshot",
			TestDescriptorCacheKeyKeepsItsCompatibilitySnapshot },
		{ "DescriptorCacheKeyTracksDescriptorRevision",
			TestDescriptorCacheKeyTracksDescriptorRevision },
		{ "TextureSamplerUpdatesUseSynchronizedSnapshot",
			TestTextureSamplerUpdatesUseSynchronizedSnapshot },
		{ "GeneratedGltfTexturesReuseTheirExistingAssetIds",
			TestGeneratedGltfTexturesReuseTheirExistingAssetIds },
		{ "VariableDescriptorCountCollectionUsesPublishedSnapshots",
			TestVariableDescriptorCountCollectionUsesPublishedSnapshots },
		{ "RenderSceneTextureCacheTracksRequestedSlotRevisions",
			TestRenderSceneTextureCacheTracksRequestedSlotRevisions },
		{ "RenderSceneTextureBindingsUseDenseLocalIndices",
			TestRenderSceneTextureBindingsUseDenseLocalIndices },
		{ "TextureSamplerCapacityReservesDefaultSlot",
			TestTextureSamplerCapacityReservesDefaultSlot },
		{ "GlobalTextureSamplerUsesFixedCapacityInPlaceUpdates",
			TestGlobalTextureSamplerUsesFixedCapacityInPlaceUpdates },
		{ "VariableDescriptorCompatibilityUsesItsFixedLayout",
			TestVariableDescriptorCompatibilityUsesItsFixedLayout },
		{ "VariableDescriptorCompatibilityRejectsDifferentLayoutCapacity",
			TestVariableDescriptorCompatibilityRejectsDifferentLayoutCapacity },
		{ "ShaderReflectionUsesDeclaredMemberOffsets",
			TestShaderReflectionUsesDeclaredMemberOffsets },
		{ "MaterialUniformUploadInitializesCompleteReflectedBlocks",
			TestMaterialUniformUploadInitializesCompleteReflectedBlocks },
		{ "MaterialLoadPromiseCoversRhiInitialization",
			TestMaterialLoadPromiseCoversRhiInitialization },
		{ "DescriptorPoolsUseSmartOwnership",
			TestDescriptorPoolsUseSmartOwnership },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.first << ": "
				<< error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
