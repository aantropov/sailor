#if defined(_WIN32)

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "RemoteViewportWindowsNative.h"

#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "Core/LogMacros.h"
#include "FrameGraph/EditorReadbackNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "GraphicsDriver/Vulkan/VulkanDeviceMemory.h"
#include "GraphicsDriver/Vulkan/VulkanFence.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Renderer.h"
#include "RHI/RenderTarget.h"
#include "RHI/Surface.h"
#include "Sailor.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>

using Microsoft::WRL::ComPtr;

// WinUI 3 uses a different native SwapChainPanel interface than UWP XAML.
// Keep the declaration local so the engine does not depend on a NuGet include path.
struct __declspec(uuid("63aad0b8-7c24-40ff-85a8-640d944cc325")) IWinUI3SwapChainPanelNative : IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE SetSwapChain(IDXGISwapChain* swapChain) = 0;
};

namespace Sailor::EditorRemote
{
	namespace
	{
		struct WindowsRendererFrameSource
		{
			RHI::RHITexturePtr m_texture{};
			std::string m_debugName{};

			bool IsValid() const
			{
				return m_texture &&
					m_texture->m_vulkan.m_image &&
					m_texture->m_vulkan.m_imageView;
			}
		};

		WindowsRendererFrameSource TryAcquireRendererFrameSource()
		{
			auto* renderer = App::GetSubmodule<RHI::Renderer>();
			FrameGraphPtr frameGraph{};
			if (renderer)
			{
				frameGraph = renderer->GetFrameGraph();
			}
			auto rhiFrameGraph = frameGraph ? frameGraph->GetRHI() : RHI::RHIFrameGraphPtr{};
			if (!rhiFrameGraph)
			{
				return {};
			}

			const auto readbackNode = rhiFrameGraph
				->GetGraphNode("EditorReadback")
				.DynamicCast<Framegraph::EditorReadbackNode>();
			if (readbackNode)
			{
				auto texture = readbackNode->GetTexture();
				if (texture && texture->m_vulkan.m_image && texture->m_vulkan.m_imageView)
				{
					WindowsRendererFrameSource source{};
					source.m_texture = texture;
					source.m_debugName = "EditorReadback";
					return source;
				}
			}

			constexpr const char* surfaceNames[] = {
				"EditorOutput",
				"Main",
				"BackBuffer",
				"Secondary"
			};
			for (const char* surfaceName : surfaceNames)
			{
				if (auto surface = rhiFrameGraph->GetSurface(surfaceName))
				{
					auto texture = surface->GetResolved();
					if (!texture)
					{
						texture = surface->GetTarget();
					}
					if (texture && texture->m_vulkan.m_image && texture->m_vulkan.m_imageView)
					{
						WindowsRendererFrameSource source{};
						source.m_texture = texture;
						source.m_debugName = std::string("Surface.") + surfaceName;
						return source;
					}
				}

				if (auto texture = rhiFrameGraph->GetRenderTarget(surfaceName))
				{
					if (texture->m_vulkan.m_image && texture->m_vulkan.m_imageView)
					{
						WindowsRendererFrameSource source{};
						source.m_texture = texture;
						source.m_debugName = std::string("RenderTarget.") + surfaceName;
						return source;
					}
				}
			}

			return {};
		}

		Failure MakeWindowsFailure(HRESULT result, const char* operation)
		{
			std::ostringstream message;
			message << operation << " failed with HRESULT=0x"
				<< std::hex << static_cast<uint32_t>(result);
			return Failure::FromDomain(
				ErrorDomain::Transport,
				static_cast<int32_t>(result),
				message.str());
		}

		Failure CreateD3D11DeviceForVulkanAdapter(
			ComPtr<ID3D11Device1>& outDevice,
			ComPtr<ID3D11DeviceContext1>& outContext,
			ComPtr<IDXGIFactory2>& outFactory)
		{
			auto* vulkanApi = Sailor::GraphicsDriver::Vulkan::VulkanApi::GetInstance();
			auto vulkanDevice = vulkanApi ? vulkanApi->GetMainDevice() : nullptr;
			if (!vulkanDevice)
			{
				return Failure::FromDomain(
					ErrorDomain::Session,
					1,
					"The Vulkan device is unavailable while creating the D3D11 device");
			}

			VkPhysicalDeviceIDProperties idProperties{};
			idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
			VkPhysicalDeviceProperties2 properties{};
			properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			properties.pNext = &idProperties;
			vkGetPhysicalDeviceProperties2(vulkanDevice->GetPhysicalDevice(), &properties);
			if (!idProperties.deviceLUIDValid)
			{
				return Failure::FromDomain(
					ErrorDomain::Capability,
					1,
					"The Vulkan adapter does not expose a Windows LUID");
			}

			ComPtr<IDXGIFactory6> factory6;
			HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory6));
			if (FAILED(result))
			{
				return MakeWindowsFailure(result, "CreateDXGIFactory1");
			}

			ComPtr<IDXGIAdapter1> matchingAdapter;
			for (UINT adapterIndex = 0; ; ++adapterIndex)
			{
				ComPtr<IDXGIAdapter1> adapter;
				result = factory6->EnumAdapters1(adapterIndex, &adapter);
				if (result == DXGI_ERROR_NOT_FOUND)
				{
					break;
				}
				if (FAILED(result))
				{
					return MakeWindowsFailure(result, "IDXGIFactory::EnumAdapters1");
				}

				DXGI_ADAPTER_DESC1 description{};
				if (SUCCEEDED(adapter->GetDesc1(&description)) &&
					std::memcmp(&description.AdapterLuid, idProperties.deviceLUID, sizeof(LUID)) == 0)
				{
					matchingAdapter = adapter;
					break;
				}
			}
			if (!matchingAdapter)
			{
				return Failure::FromDomain(
					ErrorDomain::Capability,
					1,
					"No D3D11 adapter matches the Vulkan physical-device LUID");
			}

			constexpr D3D_FEATURE_LEVEL featureLevels[] = {
				D3D_FEATURE_LEVEL_11_1,
				D3D_FEATURE_LEVEL_11_0
			};
			ComPtr<ID3D11Device> baseDevice;
			ComPtr<ID3D11DeviceContext> baseContext;
			D3D_FEATURE_LEVEL selectedFeatureLevel{};
			result = D3D11CreateDevice(
				matchingAdapter.Get(),
				D3D_DRIVER_TYPE_UNKNOWN,
				nullptr,
				D3D11_CREATE_DEVICE_BGRA_SUPPORT,
				featureLevels,
				static_cast<UINT>(std::size(featureLevels)),
				D3D11_SDK_VERSION,
				&baseDevice,
				&selectedFeatureLevel,
				&baseContext);
			if (FAILED(result))
			{
				return MakeWindowsFailure(result, "D3D11CreateDevice");
			}

			result = baseDevice.As(&outDevice);
			if (SUCCEEDED(result))
			{
				result = baseContext.As(&outContext);
			}
			if (FAILED(result))
			{
				return MakeWindowsFailure(result, "D3D11 interface upgrade");
			}

			result = matchingAdapter->GetParent(IID_PPV_ARGS(&outFactory));
			return FAILED(result)
				? MakeWindowsFailure(result, "IDXGIAdapter::GetParent")
				: Failure::Ok();
		}
	}

	struct SailorWindowsSharedSurfaceProvider::Impl
	{
		struct Allocation
		{
			WindowsViewportSurfaceKey m_key{};
			RHI::RHITexturePtr m_texture{};
			ComPtr<ID3D11Texture2D> m_ownerTexture{};
			HANDLE m_sharedHandle = nullptr;
			uint64_t m_allocationId = 0;
			FrameIndex m_frameIndex = 0;
			bool m_ownedByExternal = false;
			std::string m_lastSourceName{};

			~Allocation()
			{
				if (m_sharedHandle)
				{
					CloseHandle(m_sharedHandle);
				}
			}
		};

		TMap<WindowsViewportSurfaceKey, TUniquePtr<Allocation>> m_allocations{};
		ComPtr<ID3D11Device1> m_device{};
		ComPtr<ID3D11DeviceContext1> m_context{};
		ComPtr<IDXGIFactory2> m_factory{};
		Failure m_lastFailure = Failure::Ok();
		uint64_t m_nextAllocationId = 1;

		Failure EnsureDevice()
		{
			return m_device && m_context && m_factory
				? Failure::Ok()
				: CreateD3D11DeviceForVulkanAdapter(m_device, m_context, m_factory);
		}

		Failure CreateSharedTexture(
			uint32_t width,
			uint32_t height,
			ComPtr<ID3D11Texture2D>& outTexture,
			HANDLE& outHandle)
		{
			auto result = EnsureDevice();
			if (!result.IsOk())
			{
				return result;
			}

			D3D11_TEXTURE2D_DESC description{};
			description.Width = width;
			description.Height = height;
			description.MipLevels = 1;
			description.ArraySize = 1;
			description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			description.SampleDesc = { 1, 0 };
			description.Usage = D3D11_USAGE_DEFAULT;
			description.BindFlags =
				D3D11_BIND_RENDER_TARGET |
				D3D11_BIND_SHADER_RESOURCE;
			description.MiscFlags =
				D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
				D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

			HRESULT nativeResult = m_device->CreateTexture2D(
				&description,
				nullptr,
				&outTexture);
			if (FAILED(nativeResult))
			{
				return MakeWindowsFailure(nativeResult, "ID3D11Device::CreateTexture2D");
			}

			ComPtr<IDXGIResource1> resource;
			nativeResult = outTexture.As(&resource);
			if (FAILED(nativeResult))
			{
				return MakeWindowsFailure(nativeResult, "D3D11 texture IDXGIResource1 query");
			}

			nativeResult = resource->CreateSharedHandle(
				nullptr,
				DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
				nullptr,
				&outHandle);
			return FAILED(nativeResult)
				? MakeWindowsFailure(nativeResult, "IDXGIResource1::CreateSharedHandle")
				: Failure::Ok();
		}

		Allocation* Find(const WindowsViewportSurfaceKey& key)
		{
			auto it = m_allocations.Find(key);
			return it != m_allocations.end() ? it.Value().GetRawPtr() : nullptr;
		}

		const Allocation* Find(const WindowsViewportSurfaceKey& key) const
		{
			auto it = m_allocations.Find(key);
			return it != m_allocations.end() ? it.Value().GetRawPtr() : nullptr;
		}
	};

	SailorWindowsSharedSurfaceProvider::SailorWindowsSharedSurfaceProvider() :
		m_impl(TUniquePtr<Impl>::Make())
	{
	}

	SailorWindowsSharedSurfaceProvider::~SailorWindowsSharedSurfaceProvider() = default;

	Failure SailorWindowsSharedSurfaceProvider::CreateOrResizeSurface(
		const ViewportDescriptor& viewport,
		ConnectionEpoch epoch,
		SurfaceGeneration generation,
		WindowsViewportSurfaceState& inOutState)
	{
		auto& driver = RHI::Renderer::GetDriver();
		if (!driver)
		{
			m_impl->m_lastFailure = Failure::FromDomain(
				ErrorDomain::Session,
				1,
				"Renderer is unavailable while creating the Windows shared surface");
			return m_impl->m_lastFailure;
		}

		const RHI::ETextureUsageFlags usage =
			RHI::ETextureUsageBit::TextureTransferSrc_Bit |
			RHI::ETextureUsageBit::TextureTransferDst_Bit |
			RHI::ETextureUsageBit::Sampled_Bit;
		HANDLE sharedHandle = nullptr;
		ComPtr<ID3D11Texture2D> ownerTexture;
		auto createResult = m_impl->CreateSharedTexture(
			viewport.m_width,
			viewport.m_height,
			ownerTexture,
			sharedHandle);
		if (!createResult.IsOk())
		{
			SAILOR_LOG_ERROR("Windows shared texture creation failed: native=%d message=%s",
				createResult.m_nativeCode,
				createResult.m_message.c_str());
			m_impl->m_lastFailure = createResult;
			return createResult;
		}

		auto texture = driver->ImportD3D11Texture(
			sharedHandle,
			{ static_cast<int32_t>(viewport.m_width), static_cast<int32_t>(viewport.m_height) },
			RHI::ETextureFormat::B8G8R8A8_UNORM,
			usage,
			RHI::EImageLayout::General);
		if (!texture)
		{
			CloseHandle(sharedHandle);
			m_impl->m_lastFailure = Failure::FromDomain(
				ErrorDomain::Capability,
				1,
				"Vulkan could not import the D3D11 shared texture");
			SAILOR_LOG_ERROR("%s", m_impl->m_lastFailure.m_message.c_str());
			return m_impl->m_lastFailure;
		}

		auto allocation = TUniquePtr<Impl::Allocation>::Make();
		allocation->m_key = { viewport.m_viewportId, epoch, generation };
		allocation->m_texture = texture;
		allocation->m_ownerTexture = std::move(ownerTexture);
		allocation->m_sharedHandle = sharedHandle;
		allocation->m_allocationId = m_impl->m_nextAllocationId++;
		allocation->m_ownedByExternal = true;

		WindowsSharedSurfaceHandle nativeHandle{};
		nativeHandle.m_sharedTextureHandle = reinterpret_cast<uint64_t>(sharedHandle);
		nativeHandle.m_allocationId = allocation->m_allocationId;
		nativeHandle.m_rowPitch = viewport.m_width * 4u;

		inOutState.m_transport.m_syncMode = SyncMode::ExplicitFence;
		inOutState.m_transport.m_nativeHandles = { nativeHandle };
		const WindowsViewportSurfaceKey allocationKey = allocation->m_key;
		m_impl->m_allocations[allocationKey] = std::move(allocation);
		m_impl->m_lastFailure = Failure::Ok();
		return Failure::Ok();
	}

	Failure SailorWindowsSharedSurfaceProvider::BeginFrame(
		WindowsViewportSurfaceState& state)
	{
		auto* allocation = m_impl->Find(state.m_key);
		if (!allocation || !allocation->m_texture)
		{
			m_impl->m_lastFailure = Failure::FromDomain(
				ErrorDomain::Session,
				2,
				"Windows shared surface allocation is missing");
			return m_impl->m_lastFailure;
		}

		auto& driver = RHI::Renderer::GetDriver();
		auto* commands = RHI::Renderer::GetDriverCommands();
		if (!driver || !commands)
		{
			m_impl->m_lastFailure = Failure::FromDomain(
				ErrorDomain::Session,
				1,
				"Renderer commands are unavailable for the Windows shared surface");
			return m_impl->m_lastFailure;
		}

		const WindowsRendererFrameSource source = TryAcquireRendererFrameSource();

		auto commandList = driver->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
		commands->BeginCommandList(commandList, true);

		auto device = Sailor::GraphicsDriver::Vulkan::VulkanApi::GetInstance()->GetMainDevice();
		const uint32_t graphicsFamily = device->GetQueueFamilies().m_graphicsFamily.value();
		auto sharedImageView = allocation->m_texture->m_vulkan.m_imageView;
		if (allocation->m_ownedByExternal)
		{
			commandList->m_vulkan.m_commandBuffer->ImageMemoryBarrier(
				sharedImageView,
				sharedImageView->m_format,
				VK_IMAGE_LAYOUT_GENERAL,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				0,
				VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_QUEUE_FAMILY_EXTERNAL,
				graphicsFamily);
		}
		else
		{
			commandList->m_vulkan.m_commandBuffer->ImageMemoryBarrier(
				sharedImageView,
				sharedImageView->m_format,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				0,
				VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT);
		}

		if (source.IsValid())
		{
			commands->ImageMemoryBarrier(
				commandList,
				source.m_texture,
				source.m_texture->GetFormat(),
				source.m_texture->GetDefaultLayout(),
				RHI::EImageLayout::TransferSrcOptimal);

			const auto sourceExtent = source.m_texture->GetExtent();
			const auto destinationExtent = allocation->m_texture->GetExtent();
			const bool copied = commands->BlitImage(
				commandList,
				source.m_texture,
				allocation->m_texture,
				{ 0, 0, sourceExtent.x, sourceExtent.y },
				{ 0, 0, destinationExtent.x, destinationExtent.y });

			commands->ImageMemoryBarrier(
				commandList,
				source.m_texture,
				source.m_texture->GetFormat(),
				RHI::EImageLayout::TransferSrcOptimal,
				source.m_texture->GetDefaultLayout());
			if (!copied)
			{
				commands->EndCommandList(commandList);
				m_impl->m_lastFailure = Failure::FromDomain(
					ErrorDomain::Capability,
					1,
					"Vulkan cannot blit the renderer output into the Windows shared texture");
				return m_impl->m_lastFailure;
			}
			allocation->m_lastSourceName = source.m_debugName;
		}
		else
		{
			commands->ClearImage(commandList, allocation->m_texture, glm::vec4(0.0f));
			allocation->m_lastSourceName = "unavailable";
		}

		commandList->m_vulkan.m_commandBuffer->ImageMemoryBarrier(
			sharedImageView,
			sharedImageView->m_format,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_GENERAL,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			0,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			graphicsFamily,
			VK_QUEUE_FAMILY_EXTERNAL);

		commands->EndCommandList(commandList);
		const FrameIndex nextFrameIndex = allocation->m_frameIndex + 1;
		const uint64_t acquireKey = (nextFrameIndex - 1) * 2;
		const uint64_t releaseKey = acquireKey + 1;
		const uint32_t acquireTimeoutMs = 2000;
		VkDeviceMemory sharedMemory = *allocation->m_texture
			->m_vulkan.m_image
			->GetMemoryDevice();

		VkWin32KeyedMutexAcquireReleaseInfoKHR keyedMutexInfo{};
		keyedMutexInfo.sType = VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR;
		keyedMutexInfo.acquireCount = 1;
		keyedMutexInfo.pAcquireSyncs = &sharedMemory;
		keyedMutexInfo.pAcquireKeys = &acquireKey;
		keyedMutexInfo.pAcquireTimeouts = &acquireTimeoutMs;
		keyedMutexInfo.releaseCount = 1;
		keyedMutexInfo.pReleaseSyncs = &sharedMemory;
		keyedMutexInfo.pReleaseKeys = &releaseKey;

		auto fence = Sailor::GraphicsDriver::Vulkan::VulkanFencePtr::Make(device);
		if (!device->SubmitCommandBuffer(
				commandList->m_vulkan.m_commandBuffer,
				fence,
				{},
				{},
				&keyedMutexInfo))
		{
			m_impl->m_lastFailure = Failure::FromDomain(
				ErrorDomain::Transport,
				1,
				"Submitting the Windows shared-texture copy failed");
			return m_impl->m_lastFailure;
		}
		fence->Wait();
		allocation->m_ownedByExternal = true;
		m_impl->m_lastFailure = Failure::Ok();
		return Failure::Ok();
	}

	Failure SailorWindowsSharedSurfaceProvider::ExportFrame(
		WindowsViewportSurfaceState& state,
		FramePacket& outFrame)
	{
		auto* allocation = m_impl->Find(state.m_key);
		if (!allocation || !allocation->m_ownedByExternal)
		{
			m_impl->m_lastFailure = Failure::FromDomain(
				ErrorDomain::Session,
				2,
				"Windows shared surface is not ready for export");
			return m_impl->m_lastFailure;
		}

		outFrame = {};
		outFrame.m_viewportId = state.m_key.m_viewportId;
		outFrame.m_connectionEpoch = state.m_key.m_epoch;
		outFrame.m_generation = state.m_key.m_generation;
		outFrame.m_frameIndex = ++allocation->m_frameIndex;
		outFrame.m_width = state.m_viewport.m_width;
		outFrame.m_height = state.m_viewport.m_height;
		outFrame.m_timestampNs = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		outFrame.m_sync.m_acquireValue = outFrame.m_frameIndex * 2 - 1;
		outFrame.m_sync.m_releaseValue = outFrame.m_frameIndex * 2;
		outFrame.m_sync.m_crossApiAcquireValue = outFrame.m_sync.m_acquireValue;
		outFrame.m_sync.m_crossApiSyncKind = CrossApiSyncKind::Win32KeyedMutex;
		outFrame.m_sync.m_requiresExplicitRelease = true;
		outFrame.m_sync.m_crossApiCpuWaited = true;
		m_impl->m_lastFailure = Failure::Ok();
		return Failure::Ok();
	}

	Failure SailorWindowsSharedSurfaceProvider::ReleaseSurface(
		const WindowsViewportSurfaceState& state)
	{
		auto it = m_impl->m_allocations.Find(state.m_key);
		if (it == m_impl->m_allocations.end())
		{
			m_impl->m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		auto& allocation = *it.Value();
		if (allocation.m_sharedHandle)
		{
			CloseHandle(allocation.m_sharedHandle);
			allocation.m_sharedHandle = nullptr;
		}
		m_impl->m_allocations.Remove(state.m_key);
		m_impl->m_lastFailure = Failure::Ok();
		return Failure::Ok();
	}

	Failure SailorWindowsSharedSurfaceProvider::GetLastFailure() const
	{
		return m_impl->m_lastFailure;
	}

	std::string SailorWindowsSharedSurfaceProvider::BuildSummary(
		ViewportId viewportId,
		ConnectionEpoch epoch,
		SurfaceGeneration generation) const
	{
		const auto* allocation = m_impl->Find({ viewportId, epoch, generation });
		if (!allocation)
		{
			return "windowsSurface=missing";
		}

		std::ostringstream summary;
		summary << "windowsSurface=1 allocation=" << allocation->m_allocationId
			<< " frame=" << allocation->m_frameIndex
			<< " source='" << allocation->m_lastSourceName << "'"
			<< " externalOwned=" << (allocation->m_ownedByExternal ? 1 : 0);
		return summary.str();
	}

	struct SailorWindowsViewportPresenter::Impl
	{
		ComPtr<ID3D11Device1> m_device{};
		ComPtr<ID3D11DeviceContext1> m_context{};
		ComPtr<IDXGIFactory2> m_factory{};
		ComPtr<ID3D11Texture2D> m_sharedTexture{};
		ComPtr<IDXGIKeyedMutex> m_keyedMutex{};
		ComPtr<IDXGISwapChain1> m_swapChain{};
		ComPtr<ID3D11Query> m_copyCompleteQuery{};
		ComPtr<IWinUI3SwapChainPanelNative> m_panel{};
		ComPtr<IDXGISwapChain1> m_attachedSwapChain{};
		ViewportId m_viewportId = 0;
		ConnectionEpoch m_epoch = 0;
		SurfaceGeneration m_generation = 0;
		FrameIndex m_presentedFrameIndex = 0;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		float m_compositionScale = 1.0f;
		Failure m_lastFailure = Failure::Ok();

		Failure EnsureDevice()
		{
			if (m_device && m_context && m_factory)
			{
				return Failure::Ok();
			}

			auto createResult = CreateD3D11DeviceForVulkanAdapter(
				m_device,
				m_context,
				m_factory);
			if (!createResult.IsOk())
			{
				return createResult;
			}

			D3D11_QUERY_DESC queryDescription{};
			queryDescription.Query = D3D11_QUERY_EVENT;
			const HRESULT result = m_device->CreateQuery(
				&queryDescription,
				&m_copyCompleteQuery);
			if (FAILED(result))
			{
				return MakeWindowsFailure(result, "ID3D11Device::CreateQuery");
			}

			return Failure::Ok();
		}

		Failure AttachSwapChainOnCurrentThread()
		{
			if (!m_panel)
			{
				return Failure::FromDomain(
					ErrorDomain::Session,
					1,
					"The Windows viewport has no SwapChainPanel host");
			}
			if (!m_swapChain)
			{
				return Failure::Ok();
			}
			if (m_attachedSwapChain.Get() == m_swapChain.Get())
			{
				return Failure::Ok();
			}

			const HRESULT result = m_panel->SetSwapChain(m_swapChain.Get());
			if (FAILED(result))
			{
				return MakeWindowsFailure(result, "ISwapChainPanelNative::SetSwapChain");
			}
			m_attachedSwapChain = m_swapChain;
			return Failure::Ok();
		}

		Failure ApplyCompositionScale()
		{
			if (!m_swapChain)
			{
				return Failure::Ok();
			}

			ComPtr<IDXGISwapChain2> swapChain;
			const HRESULT queryResult = m_swapChain.As(&swapChain);
			if (FAILED(queryResult))
			{
				return MakeWindowsFailure(queryResult, "IDXGISwapChain2 query");
			}

			const float inverseScale = 1.0f / m_compositionScale;
			const DXGI_MATRIX_3X2_F transform{
				inverseScale, 0.0f,
				0.0f, inverseScale,
				0.0f, 0.0f
			};
			const HRESULT result = swapChain->SetMatrixTransform(&transform);
			return FAILED(result)
				? MakeWindowsFailure(result, "IDXGISwapChain2::SetMatrixTransform")
				: Failure::Ok();
		}
	};

	SailorWindowsViewportPresenter::SailorWindowsViewportPresenter() :
		m_impl(TUniquePtr<Impl>::Make())
	{
	}

	SailorWindowsViewportPresenter::~SailorWindowsViewportPresenter() = default;

	Failure SailorWindowsViewportPresenter::ImportSurface(
		const ViewportDescriptor& viewport,
		const TransportDescriptor& transport,
		ConnectionEpoch epoch,
		SurfaceGeneration generation)
	{
		if (transport.m_nativeHandles.empty() ||
			!transport.m_nativeHandles.front().IsValid())
		{
			m_impl->m_lastFailure = Failure::FromDomain(
				ErrorDomain::Protocol,
				1,
				"The Windows transport has no shared texture handle");
			return m_impl->m_lastFailure;
		}

		auto result = m_impl->EnsureDevice();
		if (!result.IsOk())
		{
			m_impl->m_lastFailure = result;
			return result;
		}

		m_impl->m_sharedTexture.Reset();
		m_impl->m_keyedMutex.Reset();
		const HANDLE sharedHandle = reinterpret_cast<HANDLE>(
			transport.m_nativeHandles.front().m_sharedTextureHandle);
		HRESULT nativeResult = m_impl->m_device->OpenSharedResource1(
			sharedHandle,
			IID_PPV_ARGS(&m_impl->m_sharedTexture));
		if (FAILED(nativeResult))
		{
			m_impl->m_lastFailure = MakeWindowsFailure(
				nativeResult,
				"ID3D11Device1::OpenSharedResource1");
			return m_impl->m_lastFailure;
		}

		nativeResult = m_impl->m_sharedTexture.As(&m_impl->m_keyedMutex);
		if (FAILED(nativeResult))
		{
			m_impl->m_lastFailure = MakeWindowsFailure(
				nativeResult,
				"D3D11 shared texture IDXGIKeyedMutex query");
			return m_impl->m_lastFailure;
		}

		D3D11_TEXTURE2D_DESC textureDescription{};
		m_impl->m_sharedTexture->GetDesc(&textureDescription);
		if (textureDescription.Width != viewport.m_width ||
			textureDescription.Height != viewport.m_height ||
			textureDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
		{
			m_impl->m_lastFailure = Failure::FromDomain(
				ErrorDomain::Protocol,
				1,
				"The imported D3D11 texture does not match the viewport descriptor");
			return m_impl->m_lastFailure;
		}

		DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};
		swapChainDescription.Width = viewport.m_width;
		swapChainDescription.Height = viewport.m_height;
		swapChainDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		swapChainDescription.Stereo = FALSE;
		swapChainDescription.SampleDesc = { 1, 0 };
		swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDescription.BufferCount = 2;
		swapChainDescription.Scaling = DXGI_SCALING_STRETCH;
		swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		swapChainDescription.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

		m_impl->m_swapChain.Reset();
		nativeResult = m_impl->m_factory->CreateSwapChainForComposition(
			m_impl->m_device.Get(),
			&swapChainDescription,
			nullptr,
			&m_impl->m_swapChain);
		if (FAILED(nativeResult))
		{
			m_impl->m_lastFailure = MakeWindowsFailure(
				nativeResult,
				"IDXGIFactory2::CreateSwapChainForComposition");
			return m_impl->m_lastFailure;
		}

		result = m_impl->ApplyCompositionScale();
		if (!result.IsOk())
		{
			m_impl->m_lastFailure = result;
			m_impl->m_swapChain.Reset();
			return result;
		}

		m_impl->m_attachedSwapChain.Reset();
		m_impl->m_viewportId = viewport.m_viewportId;
		m_impl->m_epoch = epoch;
		m_impl->m_generation = generation;
		m_impl->m_width = viewport.m_width;
		m_impl->m_height = viewport.m_height;
		m_impl->m_presentedFrameIndex = 0;
		m_impl->m_lastFailure = Failure::Ok();
		return Failure::Ok();
	}

	Failure SailorWindowsViewportPresenter::PresentFrame(
		ViewportId viewportId,
		const FramePacket& frame)
	{
		if (viewportId != m_impl->m_viewportId ||
			frame.m_connectionEpoch != m_impl->m_epoch ||
			frame.m_generation != m_impl->m_generation ||
			!m_impl->m_sharedTexture ||
			!m_impl->m_keyedMutex ||
			!m_impl->m_swapChain)
		{
			m_impl->m_lastFailure = Failure::FromDomain(
				ErrorDomain::Session,
				2,
				"The Windows presenter surface generation is stale");
			return m_impl->m_lastFailure;
		}

		const uint64_t acquireKey = frame.m_sync.m_acquireValue;
		const uint64_t releaseKey = frame.m_sync.m_releaseValue;
		HRESULT result = m_impl->m_keyedMutex->AcquireSync(acquireKey, 2000);
		if (FAILED(result))
		{
			m_impl->m_lastFailure = MakeWindowsFailure(result, "IDXGIKeyedMutex::AcquireSync");
			return m_impl->m_lastFailure;
		}

		if (m_impl->m_attachedSwapChain.Get() == m_impl->m_swapChain.Get())
		{
			ComPtr<ID3D11Texture2D> backBuffer;
			result = m_impl->m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
			if (FAILED(result))
			{
				m_impl->m_keyedMutex->ReleaseSync(releaseKey);
				m_impl->m_lastFailure = MakeWindowsFailure(result, "IDXGISwapChain::GetBuffer");
				return m_impl->m_lastFailure;
			}

			m_impl->m_context->CopyResource(backBuffer.Get(), m_impl->m_sharedTexture.Get());
			m_impl->m_context->End(m_impl->m_copyCompleteQuery.Get());
			m_impl->m_context->Flush();

			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (m_impl->m_context->GetData(
				m_impl->m_copyCompleteQuery.Get(),
				nullptr,
				0,
				0) == S_FALSE)
			{
				if (std::chrono::steady_clock::now() >= deadline)
				{
					m_impl->m_keyedMutex->ReleaseSync(releaseKey);
					m_impl->m_lastFailure = Failure::FromDomain(
						ErrorDomain::Session,
						1,
						"Timed out waiting for the D3D11 shared-texture copy");
					return m_impl->m_lastFailure;
				}
				std::this_thread::yield();
			}

			result = m_impl->m_swapChain->Present(1, 0);
			if (FAILED(result) && result != DXGI_STATUS_OCCLUDED)
			{
				m_impl->m_keyedMutex->ReleaseSync(releaseKey);
				m_impl->m_lastFailure = MakeWindowsFailure(result, "IDXGISwapChain::Present");
				return m_impl->m_lastFailure;
			}
		}

		result = m_impl->m_keyedMutex->ReleaseSync(releaseKey);
		if (FAILED(result))
		{
			m_impl->m_lastFailure = MakeWindowsFailure(result, "IDXGIKeyedMutex::ReleaseSync");
			return m_impl->m_lastFailure;
		}

		m_impl->m_presentedFrameIndex = frame.m_frameIndex;
		m_impl->m_lastFailure = Failure::Ok();
		return Failure::Ok();
	}

	void SailorWindowsViewportPresenter::ResetViewport(ViewportId viewportId)
	{
		if (viewportId != m_impl->m_viewportId)
		{
			return;
		}

		m_impl->m_attachedSwapChain.Reset();
		m_impl->m_swapChain.Reset();
		m_impl->m_keyedMutex.Reset();
		m_impl->m_sharedTexture.Reset();
		m_impl->m_viewportId = 0;
		m_impl->m_epoch = 0;
		m_impl->m_generation = 0;
		m_impl->m_presentedFrameIndex = 0;
		m_impl->m_width = 0;
		m_impl->m_height = 0;
		m_impl->m_lastFailure = Failure::Ok();
	}

	Failure SailorWindowsViewportPresenter::GetLastFailure() const
	{
		return m_impl->m_lastFailure;
	}

	Failure SailorWindowsViewportPresenter::BindNativeHost(
		void* swapChainPanelInspectable,
		float compositionScale)
	{
		if (!swapChainPanelInspectable)
		{
			if (m_impl->m_panel)
			{
				const HRESULT result = m_impl->m_panel->SetSwapChain(nullptr);
				if (FAILED(result))
				{
					m_impl->m_lastFailure = MakeWindowsFailure(
						result,
						"ISwapChainPanelNative::SetSwapChain(null)");
					return m_impl->m_lastFailure;
				}
			}
			m_impl->m_attachedSwapChain.Reset();
			m_impl->m_panel.Reset();
			m_impl->m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		ComPtr<IWinUI3SwapChainPanelNative> panel;
		const HRESULT result = static_cast<IUnknown*>(swapChainPanelInspectable)
			->QueryInterface(IID_PPV_ARGS(&panel));
		if (FAILED(result))
		{
			m_impl->m_lastFailure = MakeWindowsFailure(
				result,
				"SwapChainPanel QueryInterface");
			return m_impl->m_lastFailure;
		}

		if (m_impl->m_panel.Get() != panel.Get())
		{
			m_impl->m_attachedSwapChain.Reset();
			m_impl->m_panel = std::move(panel);
		}

		m_impl->m_compositionScale =
			std::isfinite(compositionScale) && compositionScale > 0.0f
				? compositionScale
				: 1.0f;
		m_impl->m_lastFailure = m_impl->ApplyCompositionScale();
		if (!m_impl->m_lastFailure.IsOk())
		{
			return m_impl->m_lastFailure;
		}

		m_impl->m_lastFailure = m_impl->AttachSwapChainOnCurrentThread();
		return m_impl->m_lastFailure;
	}

	std::string SailorWindowsViewportPresenter::BuildSummary(ViewportId viewportId) const
	{
		std::ostringstream summary;
		summary << "windowsPresenter=" << (viewportId == m_impl->m_viewportId ? 1 : 0)
			<< " panel=" << (m_impl->m_panel ? 1 : 0)
			<< " attached=" << (m_impl->m_attachedSwapChain ? 1 : 0)
			<< " size=" << m_impl->m_width << "x" << m_impl->m_height
			<< " compositionScale=" << m_impl->m_compositionScale
			<< " presentedFrame=" << m_impl->m_presentedFrameIndex;
		return summary.str();
	}
}

#endif
