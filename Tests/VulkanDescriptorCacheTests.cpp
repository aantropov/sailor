#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "GraphicsDriver/Vulkan/VulkanPipeline.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "GraphicsDriver/Vulkan/VulkanShaderModule.h"
#include "FrameGraph/RenderSceneTextureCache.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "RHI/Material.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"

#include <functional>
#include <iostream>
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

	void TestStagingAllocationIdentityKeepsEveryRange()
	{
		using Allocation = Memory::TMemoryPtr<Memory::VulkanBufferMemoryPtr>;
		using Dependency = TPair<Allocation, TWeakPtr<VulkanBufferAllocator>>;
		// An uncompiled buffer provides real pointer identity without a GPU allocation.
		const auto buffer = VulkanBufferPtr::Make(VulkanDevicePtr{}, 65536u,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE);
		const Memory::VulkanBufferMemoryPtr storage(buffer, 0u, 65536u);
		TSet<Dependency> dependencies;
		for (size_t index = 0u; index < 256u; ++index)
		{
			const Allocation range(index * 128u, 0u, 128u, storage, 0u);
			Require(dependencies.Insert(Dependency(range, {})),
				"every staging suballocation must be retained, including hash-bucket collisions");
		}
		Require(dependencies.Num() == 256u,
			"no live staging range may disappear from the command buffer release list");
		const Allocation first(0u, 0u, 128u, storage, 0u);
		Require(!dependencies.Insert(Dependency(first, {})),
			"the same staging allocation must be released exactly once");
		const Allocation aligned(0u, 16u, 112u, storage, 0u);
		Require(!Sailor::Equals(first, aligned),
			"alignment and range size are part of suballocation identity");
		const Allocation otherBlock(0u, 0u, 128u, storage, 1u);
		Require(!Sailor::Equals(first, otherBlock),
			"allocator block identity must not collapse to pointer validity");
		const Allocation shiftedStorage(0u, 0u, 128u,
			Memory::VulkanBufferMemoryPtr(buffer, 128u, 65408u), 0u);
		Require(!Sailor::Equals(first, shiftedStorage),
			"the underlying Vulkan buffer range is part of allocation identity");
		const auto otherBuffer = VulkanBufferPtr::Make(VulkanDevicePtr{}, 65536u,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE);
		const Allocation differentBuffer(0u, 0u, 128u,
			Memory::VulkanBufferMemoryPtr(otherBuffer, 0u, 65536u), 0u);
		Require(!Sailor::Equals(first, differentBuffer),
			"equal offsets in different Vulkan buffers remain different allocations");
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

	void TestConcurrentMapConstFindDoesNotExposeEndIterator()
	{
		TConcurrentMap<std::string, uint32_t> values;
		values.At_Lock("present") = 7u;
		values.Unlock("present");

		const auto& constValues = values;
		const uint32_t* present = nullptr;
		Require(constValues.Find("present", present) && present && *present == 7u,
			"const Find must return the stored value for an existing key");

		const uint32_t* missing = nullptr;
		Require(!constValues.Find("missing", missing) && missing == nullptr,
			"const Find must not expose or dereference the end iterator for a missing key");
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

	void TestDescriptorPoolsUseSmartOwnership()
	{
		Require((std::is_same_v<decltype(ThreadContext::m_descriptorPool), VulkanDescriptorPoolPtr>),
			"each Vulkan thread context must retain its descriptor pool through a smart pointer");
		Require((std::is_same_v<VulkanDescriptorSetOwnershipProbe::DescriptorPoolMemberType, VulkanDescriptorPoolPtr>),
			"descriptor allocation must receive smart descriptor-pool ownership");
		Require((std::is_same_v<VulkanDescriptorSetOwnershipProbe::DescriptorPoolPageMemberType, VulkanDescriptorPoolPagePtr>),
			"a descriptor set must retain its exact native pool page through a smart pointer");

	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "StagingAllocationIdentityKeepsEveryRange",
			TestStagingAllocationIdentityKeepsEveryRange },
		{ "SsboElementAlignmentPreservesStd430Stride",
			TestSsboElementAlignmentPreservesStd430Stride },
		{ "DescriptorCacheKeyKeepsItsCompatibilitySnapshot",
			TestDescriptorCacheKeyKeepsItsCompatibilitySnapshot },
		{ "ConcurrentMapConstFindDoesNotExposeEndIterator",
			TestConcurrentMapConstFindDoesNotExposeEndIterator },
		{ "DescriptorCacheKeyTracksDescriptorRevision",
			TestDescriptorCacheKeyTracksDescriptorRevision },
		{ "RenderSceneTextureCacheTracksRequestedSlotRevisions",
			TestRenderSceneTextureCacheTracksRequestedSlotRevisions },
		{ "RenderSceneTextureBindingsUseDenseLocalIndices",
			TestRenderSceneTextureBindingsUseDenseLocalIndices },
		{ "TextureSamplerCapacityReservesDefaultSlot",
			TestTextureSamplerCapacityReservesDefaultSlot },
		{ "VariableDescriptorCompatibilityUsesItsFixedLayout",
			TestVariableDescriptorCompatibilityUsesItsFixedLayout },
		{ "VariableDescriptorCompatibilityRejectsDifferentLayoutCapacity",
			TestVariableDescriptorCompatibilityRejectsDifferentLayoutCapacity },
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
