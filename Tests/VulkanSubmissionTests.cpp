#include "Sailor.h"
#include "Components/SkyComponent.h"
#include "FrameGraph/SkyNode.h"
#include "FrameGraph/MotionBlurNode.h"
#include "Engine/Frame.h"
#include "Engine/EngineLoop.h"
#include "Engine/GameObject.h"
#include "ECS/LightingECS.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "Support/TempDirectory.h"
#include "Support/ScopeExit.h"
#include "Support/ImGuiWorkspaceProbe.h"
#include "Platform/DynamicLibrary.h"
#include "Workspace/WorkspacePathEncoding.h"
#include "EditorEngineProtocolInternal.h"
#include "EditorEngineWebSocketServer.h"
#include "Support/EditorProtocolWire.h"
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXNetSystem.h>
#if defined(__APPLE__)
#include "Support/MacViewportPresentation.h"
#include "Support/VulkanCapabilityOverrides.h"
#endif
#include "FrameGraph/ParticlesNode.h"
#include "FrameGraph/ClearNode.h"
#include "FrameGraph/DepthHighZNode.h"
#include "FrameGraph/RenderSceneNode.h"
#include "FrameGraph/EditorReadbackNode.h"
#include "FrameGraph/CopyTextureToRamNode.h"
#include "Editor/EditorRuntimeBridge.h"
#include "Submodules/EditorRemote/RemoteViewportMacTransport.h"
#include "Submodules/ImGuiApi.h"
#include "Submodules/Editor.h"
#include "Settings/GraphicsSettings.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "GraphicsDriver/Vulkan/VulkanQueue.h"
#include "GraphicsDriver/Vulkan/VulkanFence.h"
#include "GraphicsDriver/Vulkan/VulkanSemaphore.h"
#include "GraphicsDriver/Vulkan/VulkanSwapchain.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Material.h"
#include "RHI/Mesh.h"
#include "RHI/VertexDescription.h"
#include "RHI/Renderer.h"
#include "RHI/RenderTarget.h"
#include "RHI/Surface.h"
#include "RHI/Texture.h"
#include "RHI/Cubemap.h"
#include "Tasks/Tasks.h"
#if defined(_WIN32)
#include <Windows.h>
#include "Submodules/EditorRemote/RemoteViewportWindowsNative.h"
#endif

#include <iostream>
#include <iterator>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <imgui_internal.h>

namespace { std::atomic<uint32_t> deviceIdleCalls{ 0 }; }

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
{
	static auto nativeWait = reinterpret_cast<PFN_vkDeviceWaitIdle>(dlsym(RTLD_NEXT, "vkDeviceWaitIdle"));
	++deviceIdleCalls;
	return nativeWait ? nativeWait(device) : VK_ERROR_INITIALIZATION_FAILED;
}

#endif

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace Sailor::Tests { int RunPathTracerCommandTests(int argc, const char** argv); }
namespace Sailor::Tests { int RunSkyGpu(int argc, const char** argv, bool bTestStars); }
namespace Sailor::Tests { int RunLandscapeGpu(int argc, const char** argv); }
namespace Sailor::Tests { int RunRenderContractsGpu(int argc, const char** argv, bool bTestPathTracer = false); }
namespace Sailor::Tests { void RunLoggingWithoutAppTests(); }
namespace Sailor::Tests { void RunAnimationShadowCommandTests(); }
namespace Sailor::Tests { void RunWorldLifecycleCommandTests(); }
namespace Sailor::Tests { void RunEditorMessageViewTests(); }
namespace Sailor::Tests { void RunEditorViewportCommandTests(); }
namespace Sailor::Tests { int RunEditorSimulationTests(int argc, const char** argv); }

extern "C" SAILOR_SHARED_API int32_t SailorProtocolStopLocalHost(bool bShutdownEngine) noexcept;
extern "C" SAILOR_SHARED_API int32_t SailorProtocolStartLocalHost(const uint8_t* requestData, uint32_t requestSize,
	uint16_t port, const char* token, uint32_t tokenSize) noexcept;
extern "C" SAILOR_SHARED_API void SailorProtocolRequestLocalHostStop() noexcept;
extern "C" SAILOR_SHARED_API void SailorProtocolFreeBuffer(uint8_t* buffer) noexcept;
extern "C" SAILOR_SHARED_API int32_t SailorProtocolInvoke(const uint8_t* requestData, uint32_t requestSize,
	uint8_t** responseData, uint32_t* responseSize) noexcept;

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanSubmissionTestAccess
	{
	public:
		static void ExchangeFenceDispatch(VulkanDevice& device, PFN_vkGetFenceStatus& status, PFN_vkWaitForFences& wait)
		{
			std::swap(device.m_getFenceStatus, status);
			std::swap(device.m_waitForFences, wait);
		}

		static PFN_vkQueueSubmit ExchangeSubmit(VulkanQueue& queue, PFN_vkQueueSubmit submit,
			PFN_vkQueueSubmit* forward = nullptr)
		{
			queue.m_lock.Lock();
			auto previous = std::exchange(queue.m_queueSubmit, submit);
			if (forward) *forward = previous;
			queue.m_lock.Unlock();
			return previous;
		}

		static size_t Flight(const VulkanDevice& device) { return device.m_currentFrame; }
		static bool ExchangeSwapchainOutdated(VulkanDevice& device, bool bOutdated)
		{
			return device.m_bIsSwapChainOutdated.exchange(bOutdated);
		}
		static VulkanQueuePtr PresentQueue(const VulkanDevice& device) { return device.m_presentQueue; }
		static PFN_vkQueuePresentKHR ExchangePresent(VulkanQueue& queue, PFN_vkQueuePresentKHR present)
		{
			queue.m_lock.Lock();
			const auto previous = std::exchange(queue.m_queuePresent, present);
			queue.m_lock.Unlock();
			return previous;
		}
		static void ExchangeRendering(VulkanDevice& device, PFN_vkCmdBeginRendering& begin, PFN_vkCmdEndRendering& end)
		{
			std::swap(device.pVkCmdBeginRendering, begin);
			std::swap(device.pVkCmdEndRendering, end);
		}
		static PFN_vkQueueWaitIdle ExchangeQueueWait(VulkanQueue& queue, PFN_vkQueueWaitIdle wait)
		{
			queue.m_lock.Lock();
			const auto previous = std::exchange(queue.m_queueWaitIdle, wait);
			queue.m_lock.Unlock();
			return previous;
		}
		static VulkanFencePtr FlightFence(const VulkanDevice& device) { return device.m_syncFences[device.m_currentFrame]; }
		static VulkanSemaphorePtr AcquireSemaphore(const VulkanDevice& device) { return device.m_imageAvailableSemaphores[device.m_currentFrame]; }
		static VulkanSemaphorePtr PresentSemaphore(const VulkanDevice& device) { return device.m_renderFinishedSemaphores[device.m_currentSwapchainImageIndex]; }
		static VulkanQueuePtr ComputeQueue(const VulkanDevice& device) { return device.m_computeQueue; }
		static uint32_t ImageIndex(const VulkanDevice& device) { return device.m_currentSwapchainImageIndex; }
		static VulkanQueuePtr UploadQueue(VulkanDevice& device, bool transfer = false)
		{
			const auto family = transfer ? device.m_queueFamilies.m_transferFamily : device.m_queueFamilies.m_graphicsFamily;
			if (family == device.m_queueFamilies.m_computeFamily) return device.m_computeQueue;
			if (family == device.m_queueFamilies.m_transferFamily) return device.m_transferQueue;
			return device.m_graphicsQueue;
		}
		static void CheckSyncCounts(const VulkanDevice& device)
		{
			const size_t images = device.m_swapchain->GetImageViews().Num();
			if (device.m_renderFinishedSemaphores.Num() != images || device.m_syncImages.Num() != images ||
				device.m_swapchainImagesInitialized.Num() != images || device.m_syncFences.Num() != VulkanApi::MaxFramesInFlight ||
				device.m_imageAvailableSemaphores.Num() != VulkanApi::MaxFramesInFlight || device.m_frameDeps.Num() != VulkanApi::MaxFramesInFlight)
				throw std::runtime_error("synchronization storage must follow image/flight ownership");
		}
	};
}

namespace Sailor::RHI
{
	class RendererSubmissionTestAccess
	{
	public:
		static RHISceneViewPtr View(Renderer& renderer, WorldPtr world, const RHISceneViewSnapshot& snapshot)
		{
			RHISceneViewPtr result;
			auto& views = renderer.m_cachedSceneViews.At_Lock(world);
			for (const auto& entry : views)
			{
				if (entry.m_first->m_submissionContext == snapshot.m_submissionContext)
				{
					result = entry.m_first;
					break;
				}
			}
			renderer.m_cachedSceneViews.Unlock(world);
			return result;
		}
	};
}

namespace
{
	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	thread_local VkResult nextResult = VK_SUCCESS;
	thread_local uint32_t submitCalls = 0u;
	thread_local uint32_t lastSubmitCount = 0u;
	thread_local uint32_t lastCommandCount = 0u;
	thread_local const void* lastSubmitNext = nullptr;
	thread_local bool rejectNativeSubmit = false;
	thread_local bool submitBeforeFailure = false;
	thread_local uint32_t submitsBeforeFailure = 0u;
	thread_local VkSemaphore lastWait = VK_NULL_HANDLE;
	thread_local VkSemaphore lastSignal = VK_NULL_HANDLE;
	PFN_vkQueueSubmit forwardNativeSubmit = nullptr;
	PFN_vkQueueWaitIdle forwardQueueWait = nullptr;
	thread_local VkResult queueWaitResult = VK_SUCCESS;
	thread_local uint32_t queueWaitCalls = 0u;
	PFN_vkGetFenceStatus forwardFenceStatus = nullptr;
	PFN_vkWaitForFences forwardFenceWait = nullptr;
	thread_local std::array<VkFence, 2> observedFences{};
	thread_local std::array<VkResult, 2> fenceResults{};
	thread_local uint32_t fenceStatusCalls = 0u;
	thread_local uint32_t fenceWaitCalls = 0u;
	thread_local uint32_t allFenceWaitCalls = 0u;
	thread_local VkResult fenceWaitResult = VK_TIMEOUT;
	thread_local bool captureNextFenceWait = false;
	thread_local bool capturedFenceCompleted = false;
	thread_local uint32_t waitsBeforeCapture = 0u;
	thread_local VkFence lastSubmittedFence = VK_NULL_HANDLE;
	thread_local uint32_t nativeSubmitAttempts = 0;
	PFN_vkCmdBeginRendering forwardBeginRendering = nullptr;
	PFN_vkCmdEndRendering forwardEndRendering = nullptr;
	uint32_t nativePassBegins = 0u, nativePassEnds = 0u;

	VKAPI_ATTR void VKAPI_CALL ObserveBeginRendering(VkCommandBuffer command, const VkRenderingInfo* info)
	{
		++nativePassBegins;
		forwardBeginRendering(command, info);
	}

	VKAPI_ATTR void VKAPI_CALL ObserveEndRendering(VkCommandBuffer command)
	{
		if (nativePassEnds < nativePassBegins) forwardEndRendering(command);
		++nativePassEnds;
	}

	class RenderingDispatchOverride
	{
	public:
		explicit RenderingDispatchOverride(VulkanDevice& device) : m_device(device)
		{
			VulkanSubmissionTestAccess::ExchangeRendering(device, m_begin, m_end);
			forwardBeginRendering = m_begin;
			forwardEndRendering = m_end;
			nativePassBegins = nativePassEnds = 0;
		}
		~RenderingDispatchOverride() { VulkanSubmissionTestAccess::ExchangeRendering(m_device, m_begin, m_end); }
	private:
		VulkanDevice& m_device;
		PFN_vkCmdBeginRendering m_begin = ObserveBeginRendering;
		PFN_vkCmdEndRendering m_end = ObserveEndRendering;
	};

	VKAPI_ATTR VkResult VKAPI_CALL NativeQueueWait(VkQueue queue)
	{
		// Finish actual work before reporting a synthetic shutdown failure.
		const auto result = forwardQueueWait(queue);
		++queueWaitCalls;
		return result == VK_SUCCESS ? queueWaitResult : result;
	}

	class QueueWaitOverride
	{
	public:
		QueueWaitOverride(VulkanQueuePtr queue, VkResult result) : m_queue(std::move(queue))
		{
			queueWaitResult = result;
			m_original = VulkanSubmissionTestAccess::ExchangeQueueWait(*m_queue, NativeQueueWait);
			forwardQueueWait = m_original;
		}
		~QueueWaitOverride() { VulkanSubmissionTestAccess::ExchangeQueueWait(*m_queue, m_original); }
	private:
		VulkanQueuePtr m_queue;
		PFN_vkQueueWaitIdle m_original;
	};

	VKAPI_ATTR VkResult VKAPI_CALL NativeFenceStatus(VkDevice device, VkFence fence)
	{
		for (size_t i = 0; i < observedFences.size(); ++i)
		{
			if (fence == observedFences[i])
			{
				++fenceStatusCalls;
				return fenceResults[i];
			}
		}
		return forwardFenceStatus(device, fence);
	}

	VKAPI_ATTR VkResult VKAPI_CALL NativeFenceWait(VkDevice device, uint32_t count, const VkFence* fences,
		VkBool32 all, uint64_t timeout)
	{
		++allFenceWaitCalls;
		if (count == 1u && captureNextFenceWait)
		{
			if (waitsBeforeCapture > 0u)
			{
				--waitsBeforeCapture;
				return forwardFenceWait(device, count, fences, all, timeout);
			}
			captureNextFenceWait = false;
			observedFences[0] = fences[0];
			const auto actual = forwardFenceWait(device, count, fences, all, 5000000000ull);
			capturedFenceCompleted = actual == VK_SUCCESS;
			if (!capturedFenceCompleted) return actual;
		}
		if (count == 1u && fences[0] == observedFences[0])
		{
			++fenceWaitCalls;
			return fenceWaitResult;
		}
		return forwardFenceWait(device, count, fences, all, timeout);
	}

	class FenceDispatchOverride
	{
	public:
		explicit FenceDispatchOverride(VulkanDevice& device) : m_device(device)
		{
			// App::Start is never called; all scheduler queues are drained before this scope.
			VulkanSubmissionTestAccess::ExchangeFenceDispatch(device, m_status, m_wait);
			forwardFenceStatus = m_status;
			forwardFenceWait = m_wait;
		}

		~FenceDispatchOverride()
		{
			VulkanSubmissionTestAccess::ExchangeFenceDispatch(m_device, m_status, m_wait);
			observedFences = {};
			captureNextFenceWait = false;
			waitsBeforeCapture = 0u;
		}

	private:
		VulkanDevice& m_device;
		PFN_vkGetFenceStatus m_status = NativeFenceStatus;
		PFN_vkWaitForFences m_wait = NativeFenceWait;
	};

	VKAPI_ATTR VkResult VKAPI_CALL StubSubmit(VkQueue, uint32_t count, const VkSubmitInfo*, VkFence)
	{
		++submitCalls;
		lastSubmitCount = count;
		return nextResult;
	}

	VKAPI_ATTR VkResult VKAPI_CALL NativeSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence fence)
	{
		if (rejectNativeSubmit) ++nativeSubmitAttempts;
		if (rejectNativeSubmit && submitsBeforeFailure > 0u)
		{
			--submitsBeforeFailure;
			return forwardNativeSubmit(queue, count, info, fence);
		}
		if (rejectNativeSubmit)
		{
			StubSubmit(queue, count, info, fence);
			lastSubmittedFence = fence;
			lastCommandCount = count ? info[0].commandBufferCount : 0u;
			lastSubmitNext = count ? info[0].pNext : nullptr;
			lastWait = count && info[0].waitSemaphoreCount ? info[0].pWaitSemaphores[info[0].waitSemaphoreCount - 1u] : VK_NULL_HANDLE;
			lastSignal = count && info[0].signalSemaphoreCount ? info[0].pSignalSemaphores[0] : VK_NULL_HANDLE;
			if (nextResult != VK_SUCCESS && !submitBeforeFailure) return nextResult;
		}
		const VkResult actual = forwardNativeSubmit(queue, count, info, fence);
		return rejectNativeSubmit && actual == VK_SUCCESS ? nextResult : actual;
	}

	void TestQueueDispatch()
	{
		auto native = VulkanQueuePtr::Make(VK_NULL_HANDLE, 5u, 2u);
		Require(native->QueueFamilyIndex() == 5u && native->QueueIndex() == 2u,
			"ordinary queue construction must preserve family and index without exposing loader symbols to clients");
		auto queue = VulkanQueuePtr::Make(VK_NULL_HANDLE, 3u, 1u, StubSubmit);
		VkSubmitInfo info{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		for (VkResult result : { VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST })
		{
			nextResult = result;
			const auto before = submitCalls;
			Require(queue->Submit(info) == result && submitCalls == before + 1u && lastSubmitCount == 1u,
				"single submission must forward the actual queue result");
			Require(queue->Submit(TVector<VkSubmitInfo>{ info, info }) == result &&
				submitCalls == before + 2u && lastSubmitCount == 2u,
				"batch submission must forward its complete count and queue result");
		}
	}

	class SubmitOverride
	{
	public:
		SubmitOverride(VulkanQueuePtr queue, VkResult result, bool accepted = false, uint32_t precedingSubmits = 0u) : m_queue(std::move(queue))
		{
			nextResult = result;
			submitBeforeFailure = accepted;
			submitsBeforeFailure = precedingSubmits;
			rejectNativeSubmit = true;
			m_previous = VulkanSubmissionTestAccess::ExchangeSubmit(*m_queue, NativeSubmit, &forwardNativeSubmit);
		}

		~SubmitOverride()
		{
			VulkanSubmissionTestAccess::ExchangeSubmit(*m_queue, m_previous);
			rejectNativeSubmit = false;
		}

	private:
		VulkanQueuePtr m_queue;
		PFN_vkQueueSubmit m_previous;
	};

	std::atomic<VkResult> workerSubmitResult{ VK_SUCCESS };
	std::atomic<uint32_t> workerSubmitCalls{ 0 };
	uint32_t uploadsBeforeRefusal = 0;
	std::atomic<VkFence> workerSubmittedFence{ VK_NULL_HANDLE };
	std::atomic<uint32_t> workerSubmitRefusals{ 0 };
	std::atomic<bool> workerSubmitWasOffCaller{ false };
	std::thread::id uploadCaller;
	PFN_vkQueueSubmit forwardWorkerSubmit = nullptr;

	VKAPI_ATTR VkResult VKAPI_CALL ObserveWorkerUpload(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence fence)
	{
		const auto submit = ++workerSubmitCalls;
		workerSubmittedFence.store(fence);
		const auto error = submit > uploadsBeforeRefusal ? workerSubmitResult.exchange(VK_SUCCESS) : VK_SUCCESS;
		if (error != VK_SUCCESS)
		{
			++workerSubmitRefusals;
			workerSubmitWasOffCaller.store(std::this_thread::get_id() != uploadCaller);
			return error;
		}
		return forwardWorkerSubmit(queue, count, info, fence);
	}

	class WorkerUploadOverride
	{
	public:
		explicit WorkerUploadOverride(VkResult error, bool transfer, uint32_t precedingUploads = 0) :
			m_queue(VulkanSubmissionTestAccess::UploadQueue(*VulkanApi::GetInstance()->GetMainDevice(), transfer))
		{
			workerSubmitResult.store(error);
			uploadsBeforeRefusal = precedingUploads;
			workerSubmitCalls.store(0);
			workerSubmittedFence.store(VK_NULL_HANDLE);
			workerSubmitRefusals.store(0);
			workerSubmitWasOffCaller.store(false);
			uploadCaller = std::this_thread::get_id();
			m_previous = VulkanSubmissionTestAccess::ExchangeSubmit(*m_queue, ObserveWorkerUpload, &forwardWorkerSubmit);
		}
		~WorkerUploadOverride()
		{
			VulkanSubmissionTestAccess::ExchangeSubmit(*m_queue, m_previous);
			workerSubmitResult.store(VK_SUCCESS);
		}
	private:
		VulkanQueuePtr m_queue;
		PFN_vkQueueSubmit m_previous;
	};

	struct RecordedFrame
	{
		RHICommandListPtr command;
		RHIBufferPtr readback;
		std::array<uint32_t, 64> expected;
	};

	RecordedFrame RecordFrame(uint32_t seed, VkEvent gpuGate = VK_NULL_HANDLE)
	{
		RecordedFrame frame;
		for (uint32_t i = 0; i < frame.expected.size(); ++i)
		{
			frame.expected[i] = seed + 37u * i;
		}
		frame.readback = Renderer::GetDriver()->CreateBuffer(sizeof(frame.expected), EBufferUsageBit::BufferTransferDst_Bit,
			EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
		std::fill_n(static_cast<uint32_t*>(frame.readback->GetPointer()), frame.expected.size(), 0xdeadbeefu);
		frame.command = Renderer::GetDriver()->CreateCommandList(false, ECommandListQueue::Graphics);
		auto commands = Renderer::GetDriverCommands();
		commands->BeginCommandList(frame.command, true);
		if (gpuGate != VK_NULL_HANDLE)
		{
			vkCmdWaitEvents(*frame.command->m_vulkan.m_commandBuffer, 1, &gpuGate,
				VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
		}
		frame.command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		commands->UpdateBuffer(frame.command, frame.readback, frame.expected.data(), sizeof(frame.expected));
		frame.command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
		commands->EndCommandList(frame.command);
		return frame;
	}

	void CheckReadback(RecordedFrame& frame, bool submitted)
	{
		const auto* actual = static_cast<const uint32_t*>(frame.readback->GetPointer());
		for (size_t i = 0; i < frame.expected.size(); ++i)
			Require(actual[i] == (submitted ? frame.expected[i] : 0xdeadbeefu),
				"GPU frame readback does not match accepted/rejected submission");
	}

	struct FailedFrame
	{
		RecordedFrame frame;
		VulkanFencePtr fence;
		VulkanSemaphorePtr acquire;
		VulkanSemaphorePtr finished;
	};

	void TestNativeFrameFailure(bool present, VkResult error, FailedFrame& failed)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		Require(device->WaitIdle() == VK_SUCCESS, "native setup must drain successfully");
		VulkanSubmissionTestAccess::CheckSyncCounts(*device);
		if (present)
		{
			uint32_t flight;
			bool hasImage;
			Require(device->BeginRenderSubmission(flight, hasImage) && hasImage,
				"present test must acquire a real swapchain image");
		}
		const auto flight = VulkanSubmissionTestAccess::Flight(*device);
		failed.fence = VulkanSubmissionTestAccess::FlightFence(*device);
		failed.acquire = VulkanSubmissionTestAccess::AcquireSemaphore(*device);
		failed.finished = VulkanSubmissionTestAccess::PresentSemaphore(*device);
		failed.frame = RecordFrame(713u);
		auto& command = failed.frame.command;
		const auto callsBefore = submitCalls;
		bool submitted;
		{
			SubmitOverride override(device->GetGraphicsQueue(), error);
			submitted = present ? device->PresentFrame(Sailor::FrameState{}, { command->m_vulkan.m_commandBuffer }, {}) :
				device->SubmitFrameWithoutPresent({ command->m_vulkan.m_commandBuffer }, {});
		}
		Require(!submitted && submitCalls == callsBefore + 1u, "the actual native queue call must report the injected failure");
		Require(failed.fence->Status() == VK_NOT_READY, "a rejected submission must not signal its real native fence");
		// Stop the predecessor before it can cycle back to an unsignalable fence.
		Require(VulkanSubmissionTestAccess::Flight(*device) == flight,
			"failed submission advanced the flight slot");
		Require(!device->WasLastFrameSubmitSuccessful(), "failed frame must not report successful submission");
		uint32_t nextFlight;
		bool hasNextImage;
		Require(!device->BeginRenderSubmission(nextFlight, hasNextImage),
			"failed flight must be rejected before another native fence wait or acquire");
		Require(!device->PresentFrame(Sailor::FrameState{}, {}, {}) && !device->SubmitFrameWithoutPresent({}, {}),
			"both frame paths must reject further work until recovery");
		Require(command->m_vulkan.m_commandBuffer.NumRefs() == 2u,
			"the failed flight must retain the recorded command until recovery drains it");
		CheckReadback(failed.frame, false);
	}

	void TestRecoveredFrames(bool present, FailedFrame& failed)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		VulkanSubmissionTestAccess::CheckSyncCounts(*device);
		Require(failed.fence->Status() == VK_NOT_READY && VulkanSubmissionTestAccess::FlightFence(*device) != failed.fence,
			"recovery must replace the failed fence, not signal it");
		Require(VulkanSubmissionTestAccess::AcquireSemaphore(*device) != failed.acquire &&
			VulkanSubmissionTestAccess::PresentSemaphore(*device) != failed.finished,
			"recovery must replace possibly signaled semaphores");
		Require(failed.frame.command->m_vulkan.m_commandBuffer.NumRefs() == 1u,
			"drained failed commands must no longer be retained by the device");
		TVector<VkSemaphore> imageSignals;
		imageSignals.Resize(device->GetSwapchain()->GetImageViews().Num());
		for (size_t i = 0; i < 3u * VulkanApi::MaxFramesInFlight; ++i)
		{
			uint32_t flight = static_cast<uint32_t>(VulkanSubmissionTestAccess::Flight(*device));
			bool hasImage = false;
			if (present)
				Require(device->BeginRenderSubmission(flight, hasImage) && hasImage, "recovered frame must acquire an image");
			auto fence = VulkanSubmissionTestAccess::FlightFence(*device);
			auto frame = RecordFrame(19u + static_cast<uint32_t>(i));
			VkSemaphore acquire = *VulkanSubmissionTestAccess::AcquireSemaphore(*device);
			VkSemaphore finished = *VulkanSubmissionTestAccess::PresentSemaphore(*device);
			const uint32_t imageIndex = VulkanSubmissionTestAccess::ImageIndex(*device);
			bool submitted;
			{
				SubmitOverride observe(device->GetGraphicsQueue(), VK_SUCCESS);
				submitted = present ? device->PresentFrame(Sailor::FrameState{}, { frame.command->m_vulkan.m_commandBuffer }, {}) :
					device->SubmitFrameWithoutPresent({ frame.command->m_vulkan.m_commandBuffer }, {});
			}
			Require(submitted && device->WasLastFrameSubmitSuccessful(), "recovered frame must submit real GPU work");
			Require(VulkanSubmissionTestAccess::Flight(*device) == (flight + 1u) % VulkanApi::MaxFramesInFlight,
				"successful frames must advance exactly one flight");
			Require(lastWait == (present ? acquire : VK_NULL_HANDLE) && lastSignal == (present ? finished : VK_NULL_HANDLE),
				"native submission must use the flight acquire and image presentation semaphore");
			if (present)
			{
				Require(!imageSignals[imageIndex] || imageSignals[imageIndex] == finished,
					"a reacquired image must retain its own presentation semaphore");
				imageSignals[imageIndex] = finished;
			}
			Require(fence->Wait(5000000000ull) == VK_SUCCESS, "recovered frame fence did not complete within five seconds");
			CheckReadback(frame, true);
		}
		Require(device->WaitIdle() == VK_SUCCESS, "recovered frames must drain");
	}

	void TestNativeUploadFailure(VkResult error, bool accepted)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		auto frame = RecordFrame(177u);
		auto resource = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
		auto fence = RHIFencePtr::Make();
		driver->TrackDelayedInitialization(resource.GetRawPtr(), fence);
		const auto callsBefore = submitCalls;
		bool submitted;
		{
			SubmitOverride override(VulkanSubmissionTestAccess::UploadQueue(*device), error, accepted);
			submitted = driver->SubmitCommandList(frame.command, fence);
		}
		Require(!submitted && submitCalls == callsBefore + 1u, "upload must reach the injected native submit");
		Require(fence->HasFailed() && !fence->IsFinished() && resource->HasInitializationFailed() && !resource->IsReady(),
			"a failed native upload must not publish successful initialization");
		Require(fence->m_vulkan.m_fence->Status() == (accepted ? VK_ERROR_DEVICE_LOST : VK_NOT_READY),
			"potentially submitted work must drain before failed-upload dependencies are released");
		Require(resource.NumRefs() == 1u && fence.NumRefs() == 1u && frame.command.NumRefs() == 1u,
			"a native refusal must release the upload's initialization owners");
		CheckReadback(frame, accepted);
		if (error != VK_ERROR_DEVICE_LOST)
		{
			auto replacement = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
			auto retry = RHIFencePtr::Make();
			driver->TrackDelayedInitialization(replacement.GetRawPtr(), retry);
			Require(driver->SubmitCommandList(frame.command, retry), "a rejected upload must be retryable with a fresh fence");
			retry->Wait(5000000000ull);
			Require(retry->IsFinished(), "retried upload must complete on the GPU");
			driver->TrackResources_ThreadSafe();
			Require(replacement->IsReady() && !resource->IsReady(), "retry must not resurrect the failed resource");
			CheckReadback(frame, true);
		}
	}

	template<typename Function>
	void OnRender(Function function)
	{
		auto task = Tasks::CreateTaskWithResult<std::string>("Native frame failure validation"_h, [function = std::move(function)]()
			{
				try { function(); return std::string{}; }
				catch (const std::exception& error) { return std::string(error.what()); }
			}, EThreadType::Render);
		task->Run();
		task->Wait();
		const auto error = task->GetResult();
		if (!error.empty()) throw std::runtime_error(error);
	}

	void TestConcurrentSubmissionStatistics()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto* driver = Renderer::GetDriver().GetRawPtr();
		OnRender([&]()
			{
				Require(device->SubmitFrameWithoutPresent({}, {}), "statistics setup must finish attachment initialization");
				Require(device->SubmitFrameWithoutPresent({}, {}) && driver->GetNumSubmittedCommandBuffers() == 0u,
					"an empty frame must clear earlier upload statistics");
			});

		constexpr uint32_t uploadsPerWorker = 128u;
		constexpr uint32_t frames = 32u;
		const uint32_t workers = App::GetSubmodule<Tasks::Scheduler>()->GetNumRHIThreads();
		TVector<Tasks::TaskPtr<std::string>> uploads;
		for (uint32_t worker = 0; worker < workers; ++worker)
		{
			auto task = Tasks::CreateTaskWithResult<std::string>("Concurrent native upload statistics"_h, [driver, worker]()
				{
					try
					{
						for (uint32_t i = 0; i < uploadsPerWorker; ++i)
						{
							auto upload = RecordFrame(worker * uploadsPerWorker + i);
							Require(driver->SubmitCommandList_Immediate(upload.command), "concurrent upload must complete");
							CheckReadback(upload, true);
						}
						return std::string{};
					}
					catch (const std::exception& error) { return std::string(error.what()); }
				}, EThreadType::RHI);
			uploads.Add(task);
			task->Run();
		}

		uint32_t submitted = 0;
		OnRender([&]()
			{
				for (uint32_t i = 0; i < frames; ++i)
				{
					auto first = RecordFrame(1000u + i);
					auto second = RecordFrame(2000u + i);
					auto fence = device->GetCurrentFrameFence();
					Require(device->SubmitFrameWithoutPresent(
						{ first.command->m_vulkan.m_commandBuffer, second.command->m_vulkan.m_commandBuffer }, {}),
						"statistics frame must submit both command buffers");
					submitted += driver->GetNumSubmittedCommandBuffers();
					Require(fence->Wait(5000000000ull) == VK_SUCCESS, "statistics frame must finish on the GPU");
					CheckReadback(first, true);
					CheckReadback(second, true);
				}
			});
		std::string uploadError;
		for (auto& task : uploads)
		{
			task->Wait();
			if (!task->GetResult().empty()) uploadError = task->GetResult();
		}
		Require(uploadError.empty(), uploadError.c_str());
		OnRender([&]()
			{
				Require(device->SubmitFrameWithoutPresent({}, {}), "the last frame must publish remaining uploads");
				submitted += driver->GetNumSubmittedCommandBuffers();
				Require(submitted == workers * uploadsPerWorker + frames * 2u,
					"frame boundaries must neither lose nor double-count concurrent command buffers");

				auto rejected = RecordFrame(3000u);
				auto fence = RHIFencePtr::Make();
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), VK_ERROR_OUT_OF_HOST_MEMORY);
					Require(!driver->SubmitCommandList(rejected.command, fence) && fence->HasFailed(),
						"statistics test must reach a rejected native upload");
				}
				CheckReadback(rejected, false);
				Require(device->SubmitFrameWithoutPresent({}, {}) && driver->GetNumSubmittedCommandBuffers() == 0u,
					"rejected uploads must not increment the next frame's statistics");
			});
		std::cout << "Concurrent submission statistics: " << workers * uploadsPerWorker <<
			" uploads, 32 two-command frames, readback and rejected upload passed\n";
	}

	struct RecordedEditorReadback
	{
		RHICommandListPtr command;
		RHIRenderSubmissionContextPtr context;
		VulkanFencePtr nativeFence;
		bool hasImage = false;
	};

	template<typename Node>
	RecordedEditorReadback RecordEditorReadback(Node& node,
		glm::ivec2 extent, ETextureFormat format, const void* pixels, size_t size, uint64_t generation = 0u)
	{
		auto& driver = *Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		auto texture = driver.CreateImage_Immediate(pixels, size, glm::ivec3(extent, 1), 1u, ETextureType::Texture2D, format);
		Require(texture.IsValid(), "editor readback needs a real initialized source texture");
		RecordedEditorReadback result;
		uint32_t flight;
		Require(driver.BeginRenderSubmission(flight, result.hasImage), "editor readback must acquire its flight");
		result.nativeFence = VulkanApi::GetInstance()->GetMainDevice()->GetCurrentFrameFence();
		result.context = RHIRenderSubmissionContextPtr::Make();
		result.context->BeginSubmission(1u, flight, 0u, generation);
		RHISceneViewSnapshot scene;
		scene.m_submissionContext = result.context;
		result.command = driver.CreateCommandList(false, ECommandListQueue::Graphics);
		driver.BeginCommandList(result.command, true);
		node.SetRHIResource("src"_h, texture);
		node.Process({}, {}, result.command, scene);
		driver.EndCommandList(result.command);
		return result;
	}

	FrameSubmissionResult SubmitEditorReadback(RecordedEditorReadback& recorded)
	{
		auto& driver = Renderer::GetDriver();
		auto completion = recorded.context->GetFrameCompletion();
		return recorded.hasImage ? driver->PresentFrame(Sailor::FrameState{}, { recorded.command }, {}, completion) :
			driver->SubmitFrameWithoutPresent({ recorded.command }, {}, completion);
	}

	void TestReadbackPixels()
	{
		auto& driver = Renderer::GetDriver();
		ReadbackFrame frame;
		frame.m_extent = { 2, 2 };
		frame.m_bytesPerRow = 11;
		frame.m_buffer = driver->CreateBuffer(19, EBufferUsageBit::BufferTransferDst_Bit,
			EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
		const glm::u8vec4 expected[]{ { 17, 53, 211, 0 }, { 255, 128, 0, 127 },
			{ 0, 255, 128, 255 }, { 199, 77, 31, 64 } };
		for (auto format : { ETextureFormat::R8G8B8A8_SRGB, ETextureFormat::B8G8R8A8_SRGB })
		{
			frame.m_format = format;
			auto* bytes = static_cast<uint8_t*>(frame.m_buffer->GetPointer());
			std::memset(bytes, 0xeb, 19);
			for (size_t i = 0; i < 4; ++i)
			{
				const auto rgba = expected[i];
				const auto source = format == ETextureFormat::B8G8R8A8_SRGB ? glm::u8vec4(rgba.b, rgba.g, rgba.r, rgba.a) : rgba;
				std::memcpy(bytes + (i / 2) * 11 + (i % 2) * 4, &source, 4);
			}
			TVector<glm::u8vec4> pixels;
			Require(frame.CopySrgbPixels(pixels) && pixels.Num() == 4, "padded sRGB readback must decode to tight RGBA");
			Require(frame.PrepareBgraPixels(), "the same decoder must support editor BGRA publication");
			for (size_t i = 0; i < 4; ++i)
			{
				Require(pixels[i] == expected[i], "encoded pixels and alpha must survive without a second gamma conversion");
				const auto* bgra = frame.GetBgraPixels() + (i / 2) * frame.GetBgraBytesPerRow() + (i % 2) * 4;
				Require(bgra[0] == expected[i].b && bgra[1] == expected[i].g && bgra[2] == expected[i].r && bgra[3] == expected[i].a,
					"editor channel order and row pitch must match the original pixels");
			}
		}
		frame.m_extent = { 1, 2 };
		const uint16_t halfRows[2][4]{ { 0x0000, 0x3800, 0x3c00, 0x3400 }, { 0xbc00, 0x4000, 0x3400, 0x3c00 } };
		for (size_t y = 0; y < 2; ++y)
			std::memcpy(static_cast<uint8_t*>(frame.m_buffer->GetPointer()) + y * 11, halfRows[y], 8);
		frame.m_format = ETextureFormat::R16G16B16A16_SFLOAT;
		TVector<glm::u8vec4> pixels;
		Require(frame.CopySrgbPixels(pixels) && pixels.Num() == 2, "half-float rows may begin at unaligned byte offsets");
		const glm::u8vec4 srgb[]{ { 0, 187, 255, 64 }, { 0, 255, 136, 255 } };
		for (size_t i = 0; i < 2; ++i)
		{
			for (int channel = 0; channel < 3; ++channel)
				Require(std::abs(int(pixels[i][channel]) - int(srgb[i][channel])) <= 1, "linear half-floats must encode and clamp to sRGB");
			Require(pixels[i].a == srgb[i].a, "linear alpha must not be gamma encoded");
		}
		Require(frame.PrepareBgraPixels() && frame.GetBgraBytesPerRow() == 4,
			"half-float editor output must remain tightly packed BGRA");
		const uint8_t bgraHalf[]{ 255, 128, 0, 64, 64, 255, 0, 255 };
		Require(std::memcmp(frame.GetBgraPixels(), bgraHalf, sizeof(bgraHalf)) == 0,
			"editor conversion must preserve its linear byte contract");
		frame.m_extent = { 1, 1 };
		const uint8_t linear[]{ 128, 128, 128, 128 };
		std::memcpy(frame.m_buffer->GetPointer(), linear, sizeof(linear));
		for (auto format : { ETextureFormat::R8G8B8A8_UNORM, ETextureFormat::B8G8R8A8_UNORM })
		{
			frame.m_format = format;
			Require(frame.CopySrgbPixels(pixels) && pixels[0] == glm::u8vec4(187, 187, 187, 128),
				"linear UNORM output must encode RGB without changing alpha");
		}
		frame.m_bytesPerRow = 3;
		Require(!frame.CopySrgbPixels(pixels), "short rows cannot describe a complete pixel");
		frame.m_bytesPerRow = 11;
		frame.m_extent.y = 100;
		Require(!frame.CopySrgbPixels(pixels), "decoding cannot read past the retained buffer");
		frame.m_extent.y = 1;
		frame.m_format = ETextureFormat::D32_SFLOAT;
		Require(!frame.CopySrgbPixels(pixels) && pixels[0] == glm::u8vec4(187, 187, 187, 128),
			"unsupported layouts must leave the caller's previous pixels unchanged");
	}

	void TestTextureCaptures()
	{
		Require(!App::HasEditor(), "one-shot capture must also work without the editor");
		auto node = TRefPtr<Framegraph::CopyTextureToRamNode>::Make();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		auto request = [&]()
		{
			auto task = node->DoOneCapture();
			scheduler->WaitIdle({ EThreadType::Render });
			return task;
		};
		const uint32_t firstPixels[]{ 0xff123456, 0x77123456, 0x00123456 };
		auto first = request();
		auto sameFrame = request();
		OnRender([&]()
			{
				auto recorded = RecordEditorReadback(*node, { 3, 1 }, ETextureFormat::R8G8B8A8_SRGB,
					firstPixels, sizeof(firstPixels), 11);
				node->PollCaptures();
				Require(!first->IsFinished() && !sameFrame->IsFinished(), "recording is not capture completion");
				Require(SubmitEditorReadback(recorded).m_bSubmitted && recorded.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
					"capture fixture must copy real GPU pixels");
				{
					auto device = VulkanApi::GetInstance()->GetMainDevice();
					FenceDispatchOverride dispatch(*device);
					observedFences[0] = *recorded.nativeFence;
					fenceResults[0] = VK_NOT_READY;
					node->PollCaptures();
				}
			});
		scheduler->ProcessTasksOnMainThread();
		Require(!first->IsFinished(), "an unsignalled completion must not publish a mapped capture");
		OnRender([&]() { node->PollCaptures(); });
		Require(!first->IsFinished(), "Render must publish the result through the Main task queue");
		scheduler->ProcessTasksOnMainThread();
		Require(first->IsFinished() && sameFrame->IsFinished() && first != sameFrame && first->GetResult() == sameFrame->GetResult(),
			"requests for the same rendered frame may share immutable pixels, not task identity");
		const auto retained = first->GetResult();
		Require(retained && retained->m_extent == glm::ivec2(3, 1) && retained->m_bytesPerRow == 12 && retained->m_generation == 11 &&
			std::memcmp(retained->m_buffer->GetPointer(), firstPixels, sizeof(firstPixels)) == 0,
			"capture metadata and pixels must describe the requested GPU copy");

		auto next = request();
		OnRender([&]() { node->PollCaptures(); });
		scheduler->ProcessTasksOnMainThread();
		Require(!next->IsFinished(), "an old completed image must not satisfy a new request");
		const uint32_t nextPixels[]{ 0xffeeeeee, 0xff222222 };
		OnRender([&]()
			{
				auto recorded = RecordEditorReadback(*node, { 1, 2 }, ETextureFormat::B8G8R8A8_UNORM,
					nextPixels, sizeof(nextPixels), 12);
				Require(SubmitEditorReadback(recorded).m_bSubmitted && recorded.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
					"resized capture must complete its own submission");
				node->PollCaptures();
			});
		scheduler->ProcessTasksOnMainThread();
		Require(next->IsFinished() && next->GetResult() && next->GetResult() != retained && next->GetResult()->m_generation == 12 &&
			next->GetResult()->m_extent == glm::ivec2(1, 2) && next->GetResult()->m_bytesPerRow == 4 &&
			std::memcmp(next->GetResult()->m_buffer->GetPointer(), nextPixels, sizeof(nextPixels)) == 0 &&
			std::memcmp(retained->m_buffer->GetPointer(), firstPixels, sizeof(firstPixels)) == 0,
			"resize and later GPU work must not overwrite a retained capture");

		auto cancelled = request();
		OnRender([&]() { node->Clear(); });
		scheduler->ProcessTasksOnMainThread();
		Require(cancelled->IsFinished() && !cancelled->GetResult(), "clearing the node must complete queued requests without pixels");
		auto inFlight = request();
		OnRender([&]()
			{
				auto recorded = RecordEditorReadback(*node, { 1, 2 }, ETextureFormat::B8G8R8A8_UNORM,
					nextPixels, sizeof(nextPixels));
				node->Clear();
				Require(SubmitEditorReadback(recorded).m_bSubmitted && recorded.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
					"command list ownership must retain copy resources after node cancellation");
				node->PollCaptures();
			});
		scheduler->ProcessTasksOnMainThread();
		Require(inFlight->IsFinished() && !inFlight->GetResult(), "a cancelled in-flight request must not later receive pixels");
		auto refused = request();
		OnRender([&]()
			{
				auto recorded = RecordEditorReadback(*node, { 1, 2 }, ETextureFormat::B8G8R8A8_UNORM,
					nextPixels, sizeof(nextPixels));
				SubmitOverride failure(VulkanApi::GetInstance()->GetMainDevice()->GetGraphicsQueue(), VK_ERROR_OUT_OF_HOST_MEMORY);
				Require(!SubmitEditorReadback(recorded).m_bSubmitted, "capture must observe a real refused submission");
				node->PollCaptures();
			});
		scheduler->ProcessTasksOnMainThread();
		Require(refused->IsFinished() && !refused->GetResult(), "GPU submission failure must not look like capture success");
		App::GetSubmodule<Renderer>()->FixLostDevice();
		auto abandoned = request();
		node.Clear();
		scheduler->ProcessTasksOnMainThread();
		Require(abandoned->IsFinished() && !abandoned->GetResult(), "destroying a node must resolve its pending requests");
		OnRender([]() { TestReadbackPixels(); });
		std::cout << "Texture captures: request identity, GPU completion, resize, retention, cancellation, rejection and pixel formats passed\n";
	}

#if defined(__APPLE__)
	uint32_t CheckDeviceIdleObserver(VulkanDevice& device)
	{
		const auto beforeControl = deviceIdleCalls.load();
		auto waitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(dlsym(RTLD_DEFAULT, "vkDeviceWaitIdle"));
		Require(waitIdle && waitIdle(device) == VK_SUCCESS && deviceIdleCalls == beforeControl + 1u,
			"native idle observer must see the real dynamically resolved Vulkan call");
		return deviceIdleCalls.load();
	}

	void TestMetalTextureExport()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		Require(device->IsMetalObjectsSupported(), "native texture export test requires VK_EXT_metal_objects");
		const auto idleCalls = CheckDeviceIdleObserver(*device);
		for (const auto extent : { glm::ivec2(64, 48), glm::ivec2(129, 73), glm::ivec2(1280, 720), glm::ivec2(3840, 2160) })
		{
			const uint32_t color = 0xff432100u | static_cast<uint32_t>(extent.x & 0xff);
			uint32_t flight;
			bool hasImage = false;
			Require(driver.BeginRenderSubmission(flight, hasImage), "native texture export needs an actual renderer flight");
			auto nativeFence = device->GetCurrentFrameFence();
			auto completion = RHIFencePtr::Make();
			auto command = driver.CreateCommandList(false, ECommandListQueue::Graphics);
			driver.BeginCommandList(command, true);
			auto texture = driver.CreateRenderTarget(command, extent, 1, ETextureFormat::B8G8R8A8_UNORM);
			Require(texture.IsValid(), "native texture export must allocate an actual GPU render target");
			driver.ImageMemoryBarrier(command, texture, EImageLayout::TransferDstOptimal);
			driver.ClearImage(command, texture, glm::vec4(0x43 / 255.0f, 0x21 / 255.0f, (color & 0xffu) / 255.0f, 1.0f));
			driver.ImageMemoryBarrier(command, texture, EImageLayout::General);
			driver.EndCommandList(command);
			uintptr_t exported = 0;
			Require(EditorRemote::ExportMacMetalTextureFromVulkanRenderTarget(*texture, *completion, exported).IsOk() && exported == 0,
				"unsubmitted renderer frame must not export its image");
			const auto submission = hasImage ? driver.PresentFrame(Sailor::FrameState{}, { command }, {}, completion) :
				driver.SubmitFrameWithoutPresent({ command }, {}, completion);
			Require(submission.m_bSubmitted && nativeFence->Wait(5000000000ull) == VK_SUCCESS,
				"native texture fixture must finish the actual renderer submission");
			{
				FenceDispatchOverride dispatch(*device);
				observedFences[0] = *nativeFence;
				const auto waits = allFenceWaitCalls;
				for (VkResult status : { VK_NOT_READY, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
				{
					fenceResults[0] = status;
					Require(EditorRemote::ExportMacMetalTextureFromVulkanRenderTarget(*texture, *completion, exported).IsOk() && exported == 0,
						"an unavailable renderer completion must defer export without guessing synchronization");
				}
				Require(allFenceWaitCalls == waits, "export must only poll the frame completion, never wait for a fence");
			}
			const auto begin = std::chrono::steady_clock::now();
			Require(EditorRemote::ExportMacMetalTextureFromVulkanRenderTarget(*texture, *completion, exported).IsOk() && exported != 0,
				"completed renderer frame must export its real Metal texture");
			const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
			try { Tests::CheckMacVulkanTexturePresentation(exported, extent.x, extent.y, color); }
			catch (...) { EditorRemote::ReleaseMacExportedTexture(exported); throw; }
			EditorRemote::ReleaseMacExportedTexture(exported);
			Require(deviceIdleCalls == idleCalls, "native texture export must not idle the Vulkan device");
			std::cout << "Native Vulkan texture " << extent.x << 'x' << extent.y << ": export " << elapsed << " us, 8 GPU-only presentations\n";
		}
		std::cout << "Native texture export device-idle calls: " << deviceIdleCalls - idleCalls << '\n';
	}

	void TestMetalTextureRetirement()
	{
		auto& driver = *Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		RHIFencePtr completion;
		auto createSource = [&](glm::ivec2 extent, uint32_t color)
			{
				uint32_t flight;
				bool hasImage = false;
				Require(driver.BeginRenderSubmission(flight, hasImage), "Vulkan retirement fixture needs a renderer flight");
				completion = RHIFencePtr::Make();
				auto command = driver.CreateCommandList(false, ECommandListQueue::Graphics);
				driver.BeginCommandList(command, true);
				auto texture = driver.CreateRenderTarget(command, extent, 1, ETextureFormat::B8G8R8A8_UNORM);
				driver.ImageMemoryBarrier(command, texture, EImageLayout::TransferDstOptimal);
				driver.ClearImage(command, texture, glm::vec4((color >> 16u & 0xffu) / 255.0f,
					(color >> 8u & 0xffu) / 255.0f, (color & 0xffu) / 255.0f, 1.0f));
				driver.ImageMemoryBarrier(command, texture, EImageLayout::General);
				driver.EndCommandList(command);
				const auto submission = hasImage ? driver.PresentFrame(Sailor::FrameState{}, { command }, {}, completion) :
					driver.SubmitFrameWithoutPresent({ command }, {}, completion);
				Require(submission.m_bSubmitted && completion->Wait(5000000000ull) == EFenceStatus::Finished,
					"Vulkan retirement source must finish its actual GPU clear");
				return texture;
			};
		const glm::ivec2 extents[] = { {64, 48}, {80, 56} };
		const uint32_t colors[] = { 0xff173a6cu, 0xffb25429u };
		RHITexturePtr sources[2];
		EditorRemote::MacRendererFrameSource frames[2];
		const auto idleCalls = CheckDeviceIdleObserver(*VulkanApi::GetInstance()->GetMainDevice());
		try
		{
			for (uint32_t i = 0; i < 2; ++i)
			{
				sources[i] = createSource(extents[i], colors[i]);
				auto& frame = frames[i];
				Require(EditorRemote::ExportMacMetalTextureFromVulkanRenderTarget(*sources[i], *completion, frame.m_textureObject).IsOk() &&
					frame.m_textureObject != 0, "retirement fixture must export the actual completed Vulkan image");
				frame.m_kind = EditorRemote::MacRendererFrameSourceKind::RendererOwnedMetalTexture;
				frame.m_width = extents[i].x;
				frame.m_height = extents[i].y;
				frame.m_pixelFormat = EditorRemote::PixelFormat::B8G8R8A8_UNorm;
			}
			Tests::CheckMacVulkanTextureRetirement(frames[0], colors[0], frames[1], colors[1], [&]()
				{
					for (uint32_t i = 0; i < 16; ++i) createSource(extents[i % 2], 0xffccdd00u | i);
				});
		}
		catch (...)
		{
			for (auto& frame : frames) EditorRemote::ReleaseMacExportedTexture(frame.m_textureObject);
			throw;
		}
		for (auto& frame : frames) EditorRemote::ReleaseMacExportedTexture(frame.m_textureObject);
		Require(deviceIdleCalls == idleCalls, "resize and viewport shutdown must not idle Vulkan for Metal consumption");
		std::cout << "Vulkan-source retirement: delayed copy, resize, viewport shutdown, 32 churn submissions, device-idle calls 0\n";
	}

	void CheckReadbackUpload(const EditorRemote::MacRendererFrameSource& source)
	{
		using namespace EditorRemote;
		class Source final : public IMacRendererFrameSourceProvider
		{
		public:
			MacRendererFrameSource m_frame;
			Failure AcquireFrameSource(const MacViewportSurfaceState&, FrameIndex, MacRendererFrameSource& out) override
			{
				out = m_frame;
				return Failure::Ok();
			}
		} input;
		input.m_frame = source;
		MacLoopbackIOSurfaceProvider provider(&input);
		ViewportDescriptor viewport;
		viewport.m_viewportId = 201;
		viewport.m_width = source.m_width;
		viewport.m_height = source.m_height;
		viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		viewport.m_presentMode = PresentMode::Mailbox;
		MacViewportSurfaceState state;
		Require(provider.CreateOrResizeSurface(viewport, 1, 1, state).IsOk() && provider.BeginFrame(state).IsOk(),
			"completed Vulkan readback must feed a real Metal IOSurface upload");
		FramePacket packet;
		Require(provider.ExportFrame(state, packet).IsOk() &&
			state.m_nativeAllocation->m_lastRendererSource.m_readback == source.m_readback,
			"native upload must accept and retain the actual completed readback owner");
		MacNativeSurfaceFrameEvidence evidence;
		Require(CaptureMacIOSurfaceFrameEvidence(state.m_transport.m_macSurfaces.front(), source.m_width, source.m_height,
			PixelFormat::B8G8R8A8_UNorm, evidence).IsOk(), "uploaded IOSurface must expose its actual pixels");
		const auto* pixel = source.GetCpuBytes() + (source.m_height / 2u) * source.m_bytesPerRow + (source.m_width / 2u) * 4u;
		Require(evidence.m_center.m_b == pixel[0] && evidence.m_center.m_g == pixel[1] &&
			evidence.m_center.m_r == pixel[2] && evidence.m_center.m_a == pixel[3],
			"Metal upload must preserve normalized readback channels and alpha");
		const auto copyToken = state.m_nativeAllocation->m_currentCopyToken;
		for (uint32_t i = 0; i < 100; ++i)
		{
			Require(provider.BeginFrame(state).IsOk() && provider.ExportFrame(state, packet).IsOk(),
				"the same completed GPU readback must remain exportable for presentation retry");
		}
		Require(state.m_nativeAllocation->m_currentCopyToken == copyToken &&
			state.m_nativeAllocation->m_lastProducerCopyToken == copyToken && packet.m_frameIndex == 101,
			"repeated export of an actual Vulkan readback must not upload its pixels again");
		Require(provider.ReleaseSurface(state).IsOk() && provider.GetLiveAllocationCount() == 0,
			"native readback upload fixture must release its registration");
		Tests::CheckMacReadbackPresentation(source);
	}
#endif

	void TestEditorReadback()
	{
		Require(App::HasEditor(), "the readback test must execute the actual editor node path");
		auto node = TRefPtr<Framegraph::EditorReadbackNode>::Make();
		auto* renderer = App::GetSubmodule<Renderer>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		std::vector<uint32_t> firstPixels(15);
		for (uint32_t i = 0; i < firstPixels.size(); ++i) firstPixels[i] = 0xff123456u + i;
		ReadbackFramePtr first;
		OnRender([&]()
			{
				auto recorded = RecordEditorReadback(*node, { 5, 3 }, ETextureFormat::R8G8B8A8_UNORM,
					firstPixels.data(), firstPixels.size() * sizeof(uint32_t));
				const auto completion = recorded.context->GetFrameCompletion();
				Require(completion && completion->GetStatus() == EFenceStatus::Pending && !node->TakeCompletedFrame(),
					"recording must not publish mapped pixels before submission");
				Require(SubmitEditorReadback(recorded).m_bSubmitted, "readback frame must submit");
				Require(recorded.nativeFence->Wait(5000000000ull) == VK_SUCCESS, "readback native copy did not finish");
				{
					auto device = VulkanApi::GetInstance()->GetMainDevice();
					FenceDispatchOverride dispatch(*device);
					observedFences[0] = *recorded.nativeFence;
					for (auto status : { VK_NOT_READY, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
					{
						fenceResults[0] = status;
						Require(!node->TakeCompletedFrame(), "pending or failed status queries must not expose pixels");
					}
				}
				first = node->TakeCompletedFrame();
				Require(first && first->m_completion == completion && first->m_extent == glm::ivec2(5, 3) &&
					first->m_bytesPerRow == 20u && first->m_format == ETextureFormat::R8G8B8A8_UNORM && first->m_generation == 0u,
					"completed readback metadata must belong to the same capture");
				Require(std::memcmp(first->m_buffer->GetPointer(), firstPixels.data(), firstPixels.size() * sizeof(uint32_t)) == 0,
					"readback must contain the actual source pixels");
				Require(!node->TakeCompletedFrame(), "one completion must not publish repeatedly");
				renderer->QueueEditorReadback(first);
			});
		EditorRemote::MacRendererFrameSource source;
		Require(!renderer->GetEditorReadback() && !EditorRuntime::TryAcquireEditorReadbackFrameSource(source),
			"Render must not mutate Main's published source before the handoff task runs");
		scheduler->ProcessTasksOnMainThread();
		Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.m_width == 5u && source.m_height == 3u &&
			source.m_bytesPerRow == 20u && source.m_pixelFormat == EditorRemote::PixelFormat::B8G8R8A8_UNorm &&
			source.m_readback == first && !source.m_cpuBytes, "bridge must retain the completed frame without another payload allocation");
		for (size_t i = 0; i < firstPixels.size(); ++i)
		{
			const auto* rgba = reinterpret_cast<const uint8_t*>(&firstPixels[i]);
			const auto* bgra = source.GetCpuBytes() + 4u * i;
			Require(bgra[0] == rgba[2] && bgra[1] == rgba[1] && bgra[2] == rgba[0] && bgra[3] == rgba[3],
				"bridge must convert RGBA to BGRA without changing alpha");
		}
		const auto retainedSource = source;
#if defined(__APPLE__)
		CheckReadbackUpload(source);
#endif
		Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) &&
			source.GetCpuBytes() == retainedSource.GetCpuBytes(),
			"reacquiring the same completed frame must reuse its owned pixels");
		EditorReadbackStats initialStats;
		OnRender([&]() { initialStats = node->GetStats(); });
		Require(initialStats.m_recordedReadbackBytes == 60u && initialStats.m_convertedBytes == 60u &&
			initialStats.m_bufferAllocatedBytes == 60u && initialStats.m_conversionAllocatedBytes >= 60u,
			"readback cost counters must describe the actual capture and one conversion");
		for (uint32_t i = 0; i < 100; ++i)
		{
			Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.GetCpuBytes() == retainedSource.GetCpuBytes(),
				"unchanged completed frames must not recopy or reallocate pixels");
		}
		OnRender([&]()
			{
				Require(node->GetStats().m_convertedBytes == initialStats.m_convertedBytes &&
					node->GetStats().m_conversionAllocatedBytes == initialStats.m_conversionAllocatedBytes,
					"Main acquisition must not repeat conversion or allocation");
			});
		RecordedEditorReadback pendingResize;
		OnRender([&]()
			{
				const std::vector<uint32_t> pixels(20u, 0xff331155u);
				pendingResize = RecordEditorReadback(*node, { 5, 4 }, ETextureFormat::B8G8R8A8_UNORM,
					pixels.data(), pixels.size() * sizeof(uint32_t));
				Require(!node->TakeCompletedFrame(), "a recorded resize must not replace the previous completed image");
			});
		Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.m_width == 5u && source.m_height == 3u &&
			renderer->GetEditorReadback() == first, "Main must keep old metadata while the resized frame is pending");
		OnRender([&]()
			{
				Require(SubmitEditorReadback(pendingResize).m_bSubmitted && pendingResize.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
					"resize copy must complete before simulating delayed notification");
				auto device = VulkanApi::GetInstance()->GetMainDevice();
				FenceDispatchOverride dispatch(*device);
				observedFences[0] = *pendingResize.nativeFence;
				fenceResults[0] = VK_NOT_READY;
				Require(!node->TakeCompletedFrame(), "delayed GPU completion must keep the resized capture unpublished");
				const std::vector<uint32_t> newerPixels(18u, 0xff772299u);
				auto newer = RecordEditorReadback(*node, { 6, 3 }, ETextureFormat::B8G8R8A8_UNORM,
					newerPixels.data(), newerPixels.size() * sizeof(uint32_t));
				Require(SubmitEditorReadback(newer).m_bSubmitted && newer.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
					"a different free slot must permit progress while completion notification is delayed");
				auto latest = node->TakeCompletedFrame();
				Require(latest && latest->m_extent == glm::ivec2(6, 3) &&
					std::memcmp(latest->m_buffer->GetPointer(), newerPixels.data(), newerPixels.size() * sizeof(uint32_t)) == 0,
					"late completion must not mix the new frame's pixels with earlier dimensions");
				fenceResults[0] = VK_SUCCESS;
				Require(!node->TakeCompletedFrame(), "an older delayed completion must not publish after a newer capture");
			});

		std::vector<ReadbackFramePtr> readers{ first };
		const auto capture = [&](uint32_t width, uint64_t generation = 0u)
			{
				ReadbackFramePtr frame;
				OnRender([&]()
					{
						const std::vector<uint32_t> pixels(width * 2u, 0xffabc000u + width);
						auto recorded = RecordEditorReadback(*node, { width, 2 }, ETextureFormat::B8G8R8A8_UNORM,
							pixels.data(), pixels.size() * sizeof(uint32_t), generation);
						Require(SubmitEditorReadback(recorded).m_bSubmitted && recorded.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
							"resized readback frame must finish");
						frame = node->TakeCompletedFrame();
						if (frame)
						{
							Require(frame->m_extent == glm::ivec2(width, 2) && frame->m_bytesPerRow == width * 4u &&
								std::memcmp(frame->m_buffer->GetPointer(), pixels.data(), pixels.size() * sizeof(uint32_t)) == 0,
								"resize must keep extent, row stride and pixel contents together");
							renderer->QueueEditorReadback(frame);
						}
						else Require(!recorded.context->GetFrameCompletion(), "full reader ring must skip recording, not overwrite a reader");
					});
				scheduler->ProcessTasksOnMainThread();
				return frame;
			};
		const uint32_t capacity = Renderer::GetDriver()->GetMaxFramesInFlight() + 1u;
		EditorRemote::MacRendererFrameSource retainedBgra;
		for (uint32_t i = 1; i < capacity; ++i)
		{
			auto frame = capture(7u + i);
			Require(frame.IsValid(), "readback ring must permit outstanding readers up to its capacity");
			for (const auto& reader : readers) Require(reader->m_buffer != frame->m_buffer, "held buffers must not be reused");
			readers.push_back(frame);
			if (i == 1)
			{
				Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(retainedBgra) && retainedBgra.m_readback == frame &&
					!retainedBgra.m_cpuBytes && retainedBgra.GetCpuBytes() == frame->m_buffer->GetPointer(),
					"BGRA source must borrow mapped pixels through the immutable frame owner, without copying");
#if defined(__APPLE__)
				CheckReadbackUpload(retainedBgra);
#endif
			}
		}
		Require(!capture(32u) && renderer->GetEditorReadback() == readers.back(),
			"ring pressure must retain the previous presentation without growing buffers");
		const auto releasedBuffer = readers[1]->m_buffer;
		readers[1].Clear();
		Require(!capture(32u) && *reinterpret_cast<const uint32_t*>(retainedBgra.GetCpuBytes()) == 0xffabc008u,
			"a native source owner alone must prevent reuse of its readback slot");
		retainedBgra = {};
		readers[1] = capture(6u);
		Require(readers[1] && readers[1]->m_buffer == releasedBuffer, "released completed slot should reuse its allocation");
		Require(std::memcmp(first->m_buffer->GetPointer(), firstPixels.data(), firstPixels.size() * sizeof(uint32_t)) == 0,
			"old reader pixels must survive resizing and ring reuse");
		const auto latest = renderer->GetEditorReadback();
		readers[2].Clear();
		auto otherGeneration = capture(9u, 1u);
		Require(otherGeneration && renderer->GetEditorReadback() == latest, "stale graph generations must not publish");
		OnRender([&]() { renderer->QueueEditorReadback(first); });
		scheduler->ProcessTasksOnMainThread();
		Require(renderer->GetEditorReadback() == latest, "an older capture must not replace a newer publication");
		otherGeneration.Clear();
		OnRender([&]()
			{
				const std::array<uint16_t, 8> halfPixels{ 0x3c00, 0x3800, 0, 0x3c00, 0, 0, 0x3c00, 0x3800 };
				auto recorded = RecordEditorReadback(*node, { 1, 2 }, ETextureFormat::R16G16B16A16_SFLOAT,
					halfPixels.data(), sizeof(halfPixels));
				Require(SubmitEditorReadback(recorded).m_bSubmitted && recorded.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
					"half-float readback must finish");
				auto frame = node->TakeCompletedFrame();
				Require(frame && frame->m_bytesPerRow == 8u && frame->m_format == ETextureFormat::R16G16B16A16_SFLOAT,
					"half-float source must use its real eight-byte pixel stride");
				renderer->QueueEditorReadback(frame);
			});
		scheduler->ProcessTasksOnMainThread();
		const std::array<uint8_t, 8> expectedHalf{ 0, 128, 255, 255, 255, 0, 0, 128 };
		Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.m_bytesPerRow == 4u &&
			std::memcmp(source.GetCpuBytes(), expectedHalf.data(), expectedHalf.size()) == 0,
			"half-float bridge conversion must preserve rows, channel order and alpha");
#if defined(__APPLE__)
		CheckReadbackUpload(source);
#endif
		OnRender([&]() { node->Clear(); });
		Require(std::memcmp(first->m_buffer->GetPointer(), firstPixels.data(), firstPixels.size() * sizeof(uint32_t)) == 0,
			"clearing the graph must not invalidate a retained completed reader");
		Require(std::memcmp(source.GetCpuBytes(), expectedHalf.data(), expectedHalf.size()) == 0,
			"clearing the graph must preserve a retained converted source too");

		EditorReadbackStats warmStats;
		for (uint32_t i = 0; i < 24; ++i)
		{
			OnRender([&]()
				{
					const std::vector<uint32_t> pixels(32u * 24u, 0xff123400u + i);
					auto recorded = RecordEditorReadback(*node, { 32, 24 }, ETextureFormat::R8G8B8A8_UNORM,
						pixels.data(), pixels.size() * sizeof(uint32_t));
					Require(SubmitEditorReadback(recorded).m_bSubmitted && recorded.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
						"reused conversion slot needs actual completed GPU pixels");
					auto frame = node->TakeCompletedFrame();
					Require(frame.IsValid(), "readback must continue while the previous source is retained");
					renderer->QueueEditorReadback(frame);
					if (i == 4) warmStats = node->GetStats();
					if (i > 4)
					{
						const auto& stats = node->GetStats();
						Require(stats.m_bufferAllocatedBytes == warmStats.m_bufferAllocatedBytes &&
							stats.m_conversionAllocatedBytes == warmStats.m_conversionAllocatedBytes &&
							stats.m_convertedBytes - warmStats.m_convertedBytes == (i - 4u) * 32u * 24u * 4u,
							"warm ring must reuse readback and conversion allocations while converting each new frame once");
					}
				});
			scheduler->ProcessTasksOnMainThread();
			Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.GetCpuBytes()[0] == 0x12 &&
				source.GetCpuBytes()[1] == 0x34 && source.GetCpuBytes()[2] == i && source.GetCpuBytes()[3] == 0xff,
				"reused conversion storage must contain the new frame, not stale pixels");
		}
	}

	void TestEditorReadbackRefusal(bool lost)
	{
		Require(App::HasEditor(), "refusal test requires editor readback");
		auto node = TRefPtr<Framegraph::EditorReadbackNode>::Make();
		const uint32_t pixel = 0xffabcdefu;
		OnRender([&]()
			{
				auto recorded = RecordEditorReadback(*node, { 1, 1 }, ETextureFormat::B8G8R8A8_UNORM, &pixel, sizeof(pixel));
				auto completion = recorded.context->GetFrameCompletion();
				SubmitOverride failure(VulkanApi::GetInstance()->GetMainDevice()->GetGraphicsQueue(),
					lost ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY);
				Require(!SubmitEditorReadback(recorded).m_bSubmitted && completion->HasFailed() && !node->TakeCompletedFrame(),
					"a refused native frame must fail its observer without publishing a readback");
			});
		EditorRemote::MacRendererFrameSource source;
		Require(!EditorRuntime::TryAcquireEditorReadbackFrameSource(source), "bridge must not expose refused frame bytes");
		if (lost) return;
		App::GetSubmodule<Renderer>()->FixLostDevice();
		OnRender([&]()
			{
				auto retry = RecordEditorReadback(*node, { 1, 1 }, ETextureFormat::B8G8R8A8_UNORM, &pixel, sizeof(pixel));
				Require(SubmitEditorReadback(retry).m_bSubmitted && retry.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
					"readback must recover with a new frame completion after device repair");
				auto frame = node->TakeCompletedFrame();
				Require(frame && *static_cast<const uint32_t*>(frame->m_buffer->GetPointer()) == pixel,
					"recovered readback must contain its actual pixels");
			});
	}

	thread_local VkFence pendingFlightFence = VK_NULL_HANDLE;
	thread_local std::atomic<bool>* pendingFlightWaitStarted = nullptr;

	VKAPI_ATTR VkResult VKAPI_CALL ObservePendingFlightWait(VkDevice device, uint32_t count, const VkFence* fences,
		VkBool32 all, uint64_t timeout)
	{
		if (count == 1 && fences[0] == pendingFlightFence)
		{
			pendingFlightWaitStarted->store(true);
		}
		return vkWaitForFences(device, count, fences, all, timeout);
	}

	void TestPendingFlightReuse()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		driver->WaitIdle();
		VkEvent gate = VK_NULL_HANDLE;
		const VkEventCreateInfo eventInfo{ VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
		Require(vkCreateEvent(*device, &eventInfo, nullptr, &gate) == VK_SUCCESS,
			"pending flight test requires a host-signalled GPU event");
		TVector<RecordedFrame> frames;
		TVector<RHIFencePtr> completions;
		const bool bWasOutdated = VulkanSubmissionTestAccess::ExchangeSwapchainOutdated(*device, true);
		Tests::ScopeExit cleanup([&]()
			{
				vkSetEvent(*device, gate);
				driver->WaitIdle();
				VulkanSubmissionTestAccess::ExchangeSwapchainOutdated(*device, bWasOutdated);
				vkDestroyEvent(*device, gate, nullptr);
			});
		const auto firstSlot = VulkanSubmissionTestAccess::Flight(*device);
		for (uint32_t i = 0; i < driver->GetMaxFramesInFlight(); ++i)
		{
			frames.Add(RecordFrame(1200u + i, gate));
		}
		for (auto& frame : frames)
		{
			uint32_t slot = 0;
			bool bHasImage = false;
			Require(driver->BeginRenderSubmission(slot, bHasImage) && !bHasImage,
				"pending flight fixture must acquire the no-present path");
			auto completion = RHIFencePtr::Make();
			Require(driver->SubmitFrameWithoutPresent({ frame.command }, {}, completion).m_bSubmitted,
				"the gated GPU work must be submitted, not simulated");
			completions.Add(completion);
		}
		Require(VulkanSubmissionTestAccess::Flight(*device) == firstSlot,
			"the next acquisition must recycle the first submitted flight");
		const auto fence = completions[0]->m_vulkan.m_fence;
		Require(vkGetFenceStatus(*device, *fence) == VK_NOT_READY && !completions[0]->IsFinished(),
			"the first flight must still contain pending native GPU work");
		CheckReadback(frames[0], false);

		std::atomic<bool> bWaitStarted{ false }, bAcquireReturned{ false };
		bool bObservedPendingWait = false;
		VkResult signalResult = VK_ERROR_UNKNOWN;
		pendingFlightFence = *fence;
		pendingFlightWaitStarted = &bWaitStarted;
		PFN_vkGetFenceStatus status = vkGetFenceStatus;
		PFN_vkWaitForFences wait = ObservePendingFlightWait;
		VulkanSubmissionTestAccess::ExchangeFenceDispatch(*device, status, wait);
		Tests::ScopeExit restoreDispatch([&]()
			{
				VulkanSubmissionTestAccess::ExchangeFenceDispatch(*device, status, wait);
				pendingFlightFence = VK_NULL_HANDLE;
				pendingFlightWaitStarted = nullptr;
			});
		std::jthread releaseGpu([&]()
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				while (!bWaitStarted.load() && std::chrono::steady_clock::now() < deadline)
				{
					std::this_thread::yield();
				}
				const VkFence nativeFence = *fence;
				bObservedPendingWait = bWaitStarted.load() &&
					vkWaitForFences(*device, 1, &nativeFence, VK_TRUE, 50000000ull) == VK_TIMEOUT &&
					!bAcquireReturned.load();
				signalResult = vkSetEvent(*device, gate);
			});
		uint32_t slot = 0;
		bool bHasImage = false;
		const bool bAcquired = driver->BeginRenderSubmission(slot, bHasImage);
		bAcquireReturned.store(true);
		releaseGpu.join();
		Require(bObservedPendingWait && signalResult == VK_SUCCESS && bAcquired && !bHasImage && slot == firstSlot,
			"flight acquisition must stay blocked until its real GPU work is released");
		for (size_t i = 0; i < frames.Num(); ++i)
		{
			Require(completions[i]->Wait(5000000000ull) == EFenceStatus::Finished,
				"every released flight must complete its original observer");
			CheckReadback(frames[i], true);
		}
		std::cout << "Pending GPU flight: reuse waited for the host gate; " << frames.Num()
			<< " native flights and all readback words completed\n";
	}

	void TestFrameCompletionReuse()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		TVector<RHIFencePtr> completions;
		for (uint32_t i = 0; i < 3u * driver->GetMaxFramesInFlight(); ++i)
		{
			bool hasImage;
			uint32_t flight;
			{
				FenceDispatchOverride dispatch(*device);
				observedFences[0] = *device->GetCurrentFrameFence();
				Require(forwardFenceWait(*device, 1u, observedFences.data(), VK_TRUE, 5000000000ull) == VK_SUCCESS,
					"real flight must finish before synthetic query failure");
				fenceResults[0] = VK_ERROR_OUT_OF_HOST_MEMORY;
				fenceWaitResult = VK_SUCCESS;
				Require(driver->BeginRenderSubmission(flight, hasImage), "flight acquisition must survive a later status-query error");
				if (i >= driver->GetMaxFramesInFlight())
					Require(completions[i - driver->GetMaxFramesInFlight()]->GetStatus() == EFenceStatus::Finished,
						"successful flight wait must latch old observers without relying on another status query");
			}
			auto frame = RecordFrame(50u + i);
			auto completion = RHIFencePtr::Make();
			const auto submitted = hasImage ? driver->PresentFrame(Sailor::FrameState{}, { frame.command }, {}, completion) :
				driver->SubmitFrameWithoutPresent({ frame.command }, {}, completion);
			Require(submitted.m_bSubmitted, "observed frame must submit actual work");
			Require(completion->m_vulkan.m_fence->Wait(5000000000ull) == VK_SUCCESS, "observed native flight must finish");
			CheckReadback(frame, true);
			completions.Add(completion);
			FenceDispatchOverride dispatch(*device);
			observedFences[0] = *completion->m_vulkan.m_fence;
			fenceResults[0] = VK_NOT_READY;
			if (i >= driver->GetMaxFramesInFlight())
				Require(completions[i - driver->GetMaxFramesInFlight()]->IsFinished(),
					"resetting a reused native flight must not make an older observer pending again");
		}
		TestPendingFlightReuse();
	}

	void TestAcceptedUploadLoss(bool waitForLoss)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		driver->TrackResources_ThreadSafe();
		auto first = RecordFrame(301u);
		auto second = RecordFrame(701u);
		auto shared = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
		auto firstOnly = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
		auto secondOnly = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
		auto firstFence = RHIFencePtr::Make();
		auto secondFence = RHIFencePtr::Make();
		driver->TrackDelayedInitialization(shared.GetRawPtr(), firstFence);
		driver->TrackDelayedInitialization(firstOnly.GetRawPtr(), firstFence);
		driver->TrackDelayedInitialization(shared.GetRawPtr(), secondFence);
		driver->TrackDelayedInitialization(secondOnly.GetRawPtr(), secondFence);
		try
		{
			Require(driver->SubmitCommandList(first.command, firstFence) && driver->SubmitCommandList(second.command, secondFence),
				"both uploads must be accepted before injecting a later fence error");
			// Real completion makes both the injected error and predecessor cleanup safe.
			Require(firstFence->m_vulkan.m_fence->Wait(5000000000ull) == VK_SUCCESS &&
				secondFence->m_vulkan.m_fence->Wait(5000000000ull) == VK_SUCCESS, "native uploads did not finish");
			CheckReadback(first, true);
			CheckReadback(second, true);
			FenceDispatchOverride dispatch(*device);
			observedFences = { *firstFence->m_vulkan.m_fence, *secondFence->m_vulkan.m_fence };
			for (VkResult result : { VK_NOT_READY, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				fenceResults = { result, result };
				driver->TrackResources_ThreadSafe();
				Require(first.command.NumRefs() == 2u && second.command.NumRefs() == 2u &&
					firstFence.NumRefs() == 4u && secondFence.NumRefs() == 4u && shared.NumRefs() == 3u &&
					firstOnly.NumRefs() == 2u && secondOnly.NumRefs() == 2u,
					"pending/error queries must retain every accepted upload owner");
				Require(!firstFence->HasFailed() && !secondFence->HasFailed() && !shared->IsReady(),
					"query OOM is neither upload failure nor proof of GPU completion");
			}
			fenceResults = { VK_NOT_READY, VK_NOT_READY };
			for (VkResult result : { VK_TIMEOUT, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				fenceWaitResult = result;
				Require(firstFence->Wait(0u) == EFenceStatus::Pending && !firstFence->HasFailed() && first.command.NumRefs() == 2u,
					"timeout/wait OOM must leave the upload pending with its recorded dependencies");
			}
			fenceStatusCalls = 0u;
			fenceWaitCalls = 0u;
			if (waitForLoss)
			{
				fenceWaitResult = VK_ERROR_DEVICE_LOST;
				firstFence->Wait(5000000000ull);
				fenceResults[0] = VK_SUCCESS;
			}
			else fenceResults[0] = VK_ERROR_DEVICE_LOST;
			driver->TrackResources_ThreadSafe();
			Require(firstFence->HasFailed() && !firstFence->IsFinished() && firstOnly->HasInitializationFailed(),
				"accepted upload loss was not published as terminal initialization failure");
			Require(first.command.NumRefs() == 1u && firstFence.NumRefs() == 1u && firstOnly.NumRefs() == 1u &&
				shared.NumRefs() == 2u && secondFence.NumRefs() == 4u && second.command.NumRefs() == 2u,
				"loss must retire its own owners without releasing a still-pending second upload");
			Require(!shared->IsReady() && shared->HasInitializationFailed() && !secondOnly->HasInitializationFailed(),
				"shared initialization must remember failure without poisoning pending observers");
			const auto queries = fenceStatusCalls;
			const auto waits = fenceWaitCalls;
			firstFence->Wait(0u);
			Require(!firstFence->Reset(), "a failed fence cannot be reset for reuse");
			Require(firstFence->HasFailed() && !firstFence->IsFinished() &&
				fenceStatusCalls == queries && fenceWaitCalls == waits &&
				forwardFenceStatus(*device, observedFences[0]) == VK_SUCCESS,
				"terminal failure must stay latched without resetting or re-querying its native fence");
			fenceResults[1] = VK_SUCCESS;
			driver->TrackResources_ThreadSafe();
			driver->TrackResources_ThreadSafe();
			Require(secondFence->HasFailed() && !secondFence->IsFinished() && secondOnly->HasInitializationFailed() &&
				!secondOnly->IsReady() && !shared->IsReady(), "a lost device must not publish subsequent completions as ready");
			Require(second.command.NumRefs() == 1u && secondFence.NumRefs() == 1u &&
				shared.NumRefs() == 1u && secondOnly.NumRefs() == 1u, "all completed failure owners must be released exactly once");
			auto retry = RHIFencePtr::Make();
			Require(!driver->SubmitCommandList(first.command, retry) && retry->HasFailed(),
				"loss detected by a fence must stop subsequent native submissions");
			uint32_t flight = 0u;
			bool hasImage = false;
			Require(!device->BeginRenderSubmission(flight, hasImage) &&
				!device->FixLostDevice(App::GetMainWindow().GetRawPtr()),
				"fence-reported device loss must stop new frames and cannot be repaired by recreating the swapchain");
		}
		catch (...)
		{
			device->WaitIdle();
			firstFence->MarkSubmissionFailed();
			secondFence->MarkSubmissionFailed();
			driver->TrackResources_ThreadSafe();
			throw;
		}
	}

	void TestFenceCompletionAndReuse()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		driver->TrackResources_ThreadSafe();
		auto resource = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
		auto fence = RHIFencePtr::Make();
		for (uint32_t round = 0; round < 3u; ++round)
		{
			auto frame = RecordFrame(400u + 300u * round);
			driver->TrackDelayedInitialization(resource.GetRawPtr(), fence);
			Require(!resource->IsReady() && driver->SubmitCommandList(frame.command, fence), "upload must become pending on each reuse");
			Require(fence->m_vulkan.m_fence->Wait(5000000000ull) == VK_SUCCESS, "reused native fence did not complete");
			CheckReadback(frame, true);
			{
				FenceDispatchOverride dispatch(*device);
				observedFences = { *fence->m_vulkan.m_fence, VK_NULL_HANDLE };
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				Require(fence->GetStatus() == EFenceStatus::Pending, "reset must discard cached completion");
				driver->TrackResources_ThreadSafe();
				Require(!resource->IsReady() && frame.command.NumRefs() == 2u, "pending reuse must retain commands and initialization");
				fenceResults[0] = VK_SUCCESS;
				fenceStatusCalls = 0u;
				Require(fence->IsFinished(), "successful poll must complete the fence");
				fenceResults[0] = VK_NOT_READY;
				driver->TrackResources_ThreadSafe();
				driver->TrackResources_ThreadSafe();
				Require(fence->Wait(0u) == EFenceStatus::Finished && fence->IsFinished() && fenceStatusCalls == 1u,
					"all completion consumers must share one terminal result, not re-poll the driver");
				Require(resource->IsReady() && !resource->HasInitializationFailed() && resource.NumRefs() == 1u &&
					fence.NumRefs() == 1u && frame.command.NumRefs() == 1u, "successful completion must release all initialization owners");
			}
			Require(fence->Reset() && fence->m_vulkan.m_fence->Status() == VK_NOT_READY && !fence->IsFinished(),
				"successful reset must clear both native and cached completion");
		}
	}

	void TestImmediateSubmission(bool lost)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		auto success = RecordFrame(811u);
		const bool completed = driver->SubmitCommandList_Immediate(success.command);
		Require(completed && success.command.NumRefs() == 1u,
			"immediate success must report completion and release its command owner");
		CheckReadback(success, true);
		for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			if (!lost)
			{
				auto rejected = RecordFrame(911u);
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
					const bool accepted = driver->SubmitCommandList_Immediate(rejected.command);
					Require(!accepted && rejected.command.NumRefs() == 1u,
						"immediate refusal must report failure without retaining an unsubmitted command");
				}
				CheckReadback(rejected, false);
				Require(driver->SubmitCommandList_Immediate(rejected.command), "a refused immediate command must remain retryable");
				CheckReadback(rejected, true);
			}
			auto pending = RecordFrame(1011u);
			{
				FenceDispatchOverride dispatch(*device);
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : error;
				captureNextFenceWait = true;
				capturedFenceCompleted = false;
				fenceWaitCalls = 0u;
				Require(!driver->SubmitCommandList_Immediate(pending.command) && capturedFenceCompleted && fenceWaitCalls == 1u,
					"an unsuccessful immediate wait must reach the caller as failure");
				Require(pending.command.NumRefs() == (lost ? 1u : 2u),
					"immediate failure must retire terminal loss but retain a pending command after wait OOM");
				if (!lost)
				{
					driver->TrackResources_ThreadSafe();
					Require(pending.command.NumRefs() == 2u, "pending immediate ownership must survive repeated collection");
					fenceResults[0] = VK_SUCCESS;
					driver->TrackResources_ThreadSafe();
					Require(pending.command.NumRefs() == 1u, "later completion must release the pending immediate command");
				}
			}
			CheckReadback(pending, true);
			if (lost)
			{
				Require(!driver->SubmitCommandList_Immediate(pending.command), "a lost device must refuse later immediate work");
				break;
			}
		}
	}

	void TestExtendedSubmission(bool lost)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = static_cast<VulkanGraphicsDriver&>(*Renderer::GetDriver());
		VkProtectedSubmitInfo extension{ VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO };
		extension.protectedSubmit = VK_FALSE;
		for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			auto frame = RecordFrame(2251u);
			auto fence = RHIFencePtr::Make();
			if (!lost)
			{
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
					const bool submitted = driver.SubmitCommandList(frame.command, fence, {}, {}, &extension);
					Require(!submitted, "extended submission must report native refusal");
					Require(lastSubmitNext == &extension, "refused submission must forward its native extension chain");
					Require(fence->HasFailed() && frame.command.NumRefs() == 1u,
						"extended submit refusal must fail its fence and release the command");
				}
				CheckReadback(frame, false);
				fence = RHIFencePtr::Make();
			}
			{
				SubmitOverride observe(VulkanSubmissionTestAccess::UploadQueue(*device), VK_SUCCESS);
				FenceDispatchOverride dispatch(*device);
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : error;
				captureNextFenceWait = true;
				capturedFenceCompleted = false;
				Require(driver.SubmitCommandList(frame.command, fence, {}, {}, &extension) && lastSubmitNext == &extension,
					"extended submission must preserve its native extension chain");
				Require(fence->Wait() == (lost ? EFenceStatus::Failed : EFenceStatus::Pending) && capturedFenceCompleted,
					"extended fence must report actual wait failure after the real test copy finishes");
				driver.TrackResources_ThreadSafe();
				Require(frame.command.NumRefs() == (lost ? 1u : 2u),
					"extended work must share ordinary command retention and terminal-loss cleanup");
				if (!lost)
				{
					driver.TrackResources_ThreadSafe();
					Require(frame.command.NumRefs() == 2u, "pending extended submission must survive collection");
					fenceResults[0] = VK_SUCCESS;
					driver.TrackResources_ThreadSafe();
					Require(fence->IsFinished() && frame.command.NumRefs() == 1u, "extended completion must release command ownership");
				}
			}
			CheckReadback(frame, true);
			if (lost) break;
		}
	}

#if defined(_WIN32)
	void TestWindowsSharedSurfaceCopy(bool lost)
	{
		using namespace Sailor::EditorRemote;
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver();
		SailorWindowsSharedSurfaceProvider provider;
		WindowsViewportTransportBackend backend(provider);
		SailorWindowsViewportPresenter presenter;
		ViewportDescriptor viewport;
		viewport.m_viewportId = 96u;
		viewport.m_width = viewport.m_height = 16u;
		viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		TransportDescriptor transport;
		Require(backend.EnsureSurface(viewport, 1u, 1u, transport).IsOk(), "native Windows shared surface creation failed");
		Require(presenter.ImportSurface(viewport, transport, 1u, 1u).IsOk(), "native D3D presenter import failed");
		FrameIndex exported = 0u;
		auto exportAndRelease = [&]()
			{
				FramePacket frame;
				Require(backend.ExportFrame(viewport, 1u, 1u, frame).IsOk() && frame.m_frameIndex == ++exported &&
					frame.m_sync.m_acquireValue == exported * 2u - 1u && frame.m_sync.m_releaseValue == exported * 2u &&
					frame.m_sync.m_crossApiCpuWaited, "completed Windows copy must export exactly the accepted keyed-mutex frame");
				if (exported == 1u)
				{
					auto unavailable = frame;
					unavailable.m_sync.m_acquireValue += 2u;
					const auto result = presenter.PresentFrame(viewport.m_viewportId, unavailable);
					Require(!result.IsOk() && result.m_nativeCode == WAIT_TIMEOUT,
						"an unavailable mutex key must report timeout without consuming the ready frame");
				}
				Require(presenter.PresentFrame(viewport.m_viewportId, frame).IsOk(), "D3D must acquire and release the completed frame");
				Require(!backend.ExportFrame(viewport, 1u, 1u, frame).IsOk(), "completed copy must export only once");
			};
		for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			if (!lost)
			{
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
					Require(!backend.BeginFrame(viewport, 1u, 1u).IsOk(), "Windows begin must report submit refusal");
				}
				Require(backend.BeginFrame(viewport, 1u, 1u).IsOk(), "refused Windows copy must retry successfully");
				exportAndRelease();
			}
			{
				SubmitOverride observe(VulkanSubmissionTestAccess::UploadQueue(*device), VK_SUCCESS);
				FenceDispatchOverride dispatch(*device);
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : error;
				captureNextFenceWait = true;
				capturedFenceCompleted = false;
				Require(!backend.BeginFrame(viewport, 1u, 1u).IsOk() && capturedFenceCompleted,
					"Windows begin must report an accepted wait failure");
				const auto submitted = submitCalls;
				FramePacket frame;
				auto state = *std::as_const(backend).FindSurface(viewport.m_viewportId, 1u, 1u);
				Require(!provider.ExportFrame(state, frame).IsOk() && !backend.ExportFrame(viewport, 1u, 1u, frame).IsOk(),
					"neither provider nor backend may publish an incomplete Windows copy");
				Require(!backend.BeginFrame(viewport, 1u, 1u).IsOk() && submitCalls == submitted,
					"pending or lost Windows copy must not resubmit using its previous mutex key");
				if (!lost)
				{
					fenceWaitResult = VK_SUCCESS;
					fenceResults[0] = VK_SUCCESS;
					Require(backend.BeginFrame(viewport, 1u, 1u).IsOk() && submitCalls == submitted,
						"Windows retry must finish the original accepted copy");
					exportAndRelease();
				}
			}
			if (lost) break;
		}
		presenter.ResetViewport(viewport.m_viewportId);
		Require(backend.ReleaseSurface(viewport.m_viewportId, 1u, 1u).IsOk(), "Windows surface release failed");
		if (!lost)
		{
			Require(backend.EnsureSurface(viewport, 1u, 2u, transport).IsOk(), "replacement Windows surface creation failed");
			FenceDispatchOverride dispatch(*device);
			fenceResults = { VK_NOT_READY, VK_NOT_READY };
			fenceWaitResult = VK_ERROR_OUT_OF_HOST_MEMORY;
			captureNextFenceWait = true;
			capturedFenceCompleted = false;
			Require(!backend.BeginFrame(viewport, 1u, 2u).IsOk() && capturedFenceCompleted, "replacement copy must enter pending state");
			Require(backend.ReleaseSurface(viewport.m_viewportId, 1u, 2u).IsOk(), "pending surface must be releasable");
			Require(forwardFenceStatus(*device, observedFences[0]) == VK_SUCCESS,
				"releasing the allocation must not destroy the tracked native fence");
			fenceResults[0] = VK_SUCCESS;
			driver.TrackResources_ThreadSafe();
		}
	}
#endif

	void TestImmediateBindingUpdate(bool lost)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		std::array<uint32_t, 64> data{};
		for (uint32_t i = 0; i < data.size(); ++i) data[i] = 171u + 43u * i;
		auto buffer = driver->CreateBuffer(sizeof(data), EBufferUsageBit::UniformBuffer_Bit |
			EBufferUsageBit::BufferTransferSrc_Bit | EBufferUsageBit::BufferTransferDst_Bit, EMemoryPropertyBit::DeviceLocal);
		auto bindings = driver->CreateShaderBindings();
		driver->AddBufferToShaderBindings(bindings, buffer, "immediate"_h, 0u);
		auto checkData = [&]()
			{
				auto readback = driver->CreateBuffer(sizeof(data), EBufferUsageBit::BufferTransferDst_Bit,
					EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
				auto command = driver->CreateCommandList(false, ECommandListQueue::Transfer);
				commands->BeginCommandList(command, true);
				auto& native = command->m_vulkan.m_commandBuffer;
				native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
				native->CopyBuffer(*buffer->m_vulkan.m_buffer->Get(), *readback->m_vulkan.m_buffer->Get(), sizeof(data));
				native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
				commands->EndCommandList(command);
				Require(driver->SubmitCommandList_Immediate(command), "binding readback did not complete");
				const auto* actual = static_cast<const uint32_t*>(readback->GetPointer());
				Require(std::equal(data.begin(), data.end(), actual), "immediate binding contents mismatch");
			};
		Require(driver->UpdateShaderBinding_Immediate(bindings, "immediate"_h, data.data(), sizeof(data)),
			"successful binding upload must report completion");
		checkData();
		for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			if (!lost)
			{
				auto refused = data;
				refused.fill(0xdeadbeefu);
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device, true), error);
					Require(!driver->UpdateShaderBinding_Immediate(bindings, "immediate"_h, refused.data(), sizeof(refused)),
						"binding update must propagate submit refusal");
				}
				checkData();
			}
			for (auto& value : data) value += 121u;
			{
				FenceDispatchOverride dispatch(*device);
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : error;
				captureNextFenceWait = true;
				capturedFenceCompleted = false;
				Require(!driver->UpdateShaderBinding_Immediate(bindings, "immediate"_h, data.data(), sizeof(data)) && capturedFenceCompleted,
					"binding update must propagate accepted wait failure");
				if (!lost)
				{
					fenceResults[0] = VK_SUCCESS;
					driver->TrackResources_ThreadSafe();
				}
			}
			if (lost) break;
			checkData();
		}
	}

	void TestImmediateBufferCreation(bool lost = false)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		std::array<uint32_t, 64> data{};
		data.fill(0x17bac012u);
		FenceDispatchOverride dispatch(*device);
		fenceResults = { VK_NOT_READY, VK_NOT_READY };
		fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY;
		captureNextFenceWait = true;
		capturedFenceCompleted = false;
		auto buffer = driver->CreateBuffer_Immediate(data.data(), sizeof(data), EBufferUsageBit::StorageBuffer_Bit);
		fenceResults[0] = VK_SUCCESS;
		driver->TrackResources_ThreadSafe();
		Require(capturedFenceCompleted, "buffer creation fixture must complete native work before injecting wait failure");
		Require(!buffer, "an incomplete immediate upload must not publish a usable buffer");
	}

	void TestImmediateImageCreation(bool lost)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		std::array<uint32_t, 16> data{};
		for (uint32_t i = 0; i < data.size(); ++i) data[i] = 0xff100000u + 739u * i;
		FenceDispatchOverride dispatch(*device);
		fenceResults = { VK_NOT_READY, VK_NOT_READY };
		fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY;
		captureNextFenceWait = true;
		capturedFenceCompleted = false;
		auto texture = driver->CreateImage_Immediate(data.data(), sizeof(data), glm::ivec3(4, 4, 1));
		fenceResults[0] = VK_SUCCESS;
		driver->TrackResources_ThreadSafe();
		Require(capturedFenceCompleted, "image fixture must finish native work before injecting wait failure");
		Require(!texture, "an incomplete immediate upload must not publish a usable texture");
	}

	std::vector<uint32_t> ReadImage(IGraphicsDriver& driver, VulkanImagePtr image, uint32_t mip = 0u, uint32_t layer = 0u)
	{
		const uint32_t width = std::max(1u, image->m_extent.width >> mip);
		const uint32_t height = std::max(1u, image->m_extent.height >> mip);
		const uint32_t depth = std::max(1u, image->m_extent.depth >> mip);
		std::vector<uint32_t> result(width * height * depth);
		auto buffer = driver.CreateBuffer(result.size() * sizeof(uint32_t), EBufferUsageBit::BufferTransferDst_Bit,
			EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
		auto command = driver.CreateCommandList(false, ECommandListQueue::Graphics);
		auto& native = command->m_vulkan.m_commandBuffer;
		native->BeginCommandList(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
		native->ImageMemoryBarrier(image, image->m_format, image->m_defaultLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		native->CopyImageToBuffer(*buffer->m_vulkan.m_buffer->Get(), image, width, height, depth, mip, layer);
		native->ImageMemoryBarrier(image, image->m_format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image->m_defaultLayout);
		native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
		native->EndCommandList();
		Require(driver.SubmitCommandList_Immediate(command), "image readback did not complete");
		std::copy_n(static_cast<const uint32_t*>(buffer->GetPointer()), result.size(), result.begin());
		return result;
	}

	void TestMsaaCacheRefusal()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		uint32_t caseIndex = 0;
		for (auto format : { ETextureFormat::R8G8B8A8_UNORM, ETextureFormat::D32_SFLOAT })
		{
			for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				const glm::ivec2 extent(43 + caseIndex++, 27);
				RHITexturePtr rejected;
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
					const auto before = submitCalls;
					rejected = driver.GetOrAddMsaaFramebufferRenderTarget(format, extent);
					Require(submitCalls == before + 1u && lastCommandCount == 1u,
						"the MSAA fixture must reject the target's actual initialization command");
				}
				RHITexturePtr retry;
				{
					FenceDispatchOverride completion(*device);
					{
						SubmitOverride accepted(VulkanSubmissionTestAccess::UploadQueue(*device), VK_SUCCESS);
						retry = driver.GetOrAddMsaaFramebufferRenderTarget(format, extent);
						observedFences[0] = lastSubmittedFence;
						fenceResults[0] = VK_NOT_READY;
					}
					std::cout << "MSAA cache refusal: format=" << static_cast<uint32_t>(format) << ", error=" << error
						<< ", rejectedPublished=" << static_cast<bool>(rejected) << ", retryReusedRejected=" << (retry == rejected) << '\n';
					Require(retry && retry != rejected && !retry->HasInitializationFailed(),
						"a rejected MSAA initialization must not poison the same-key cache retry");
					Require(!rejected, "a refused MSAA initialization must not publish an attachment");
					Require(driver.GetOrAddMsaaFramebufferRenderTarget(format, extent) == retry,
						"an accepted pending MSAA target must retain its warm cache identity");
					driver.TrackResources_ThreadSafe();
					Require(!retry->IsReady() && !retry->HasInitializationFailed(),
						"pending MSAA work must remain owned without false readiness or a replacement");
					Require(forwardFenceWait(*device, 1, &observedFences[0], VK_TRUE, 5000000000ull) == VK_SUCCESS,
						"the accepted MSAA initialization must complete on the native GPU");
					fenceResults[0] = VK_SUCCESS;
					driver.TrackResources_ThreadSafe();
					Require(retry->IsReady() && driver.GetOrAddMsaaFramebufferRenderTarget(format, extent) == retry,
						"completion must keep the original MSAA cache identity");
				}

				const bool depth = IsDepthFormat(format);
				auto command = driver.CreateCommandList(false, ECommandListQueue::Graphics);
				driver.BeginCommandList(command, true);
				const auto usage = (depth ? ETextureUsageBit::DepthStencilAttachment_Bit : ETextureUsageBit::ColorAttachment_Bit) |
					ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit;
				auto resolved = driver.CreateRenderTarget(command, extent, 1, format, ETextureFiltration::Nearest, ETextureClamping::Clamp, usage);
				if (depth) driver.ImageMemoryBarrier(command, resolved, EImageLayout::DepthAttachmentOptimal);
				TVector<VulkanImageViewPtr> colors, resolves;
				if (!depth)
				{
					colors.Add(retry->m_vulkan.m_imageView);
					resolves.Add(resolved->m_vulkan.m_imageView);
				}
				command->m_vulkan.m_commandBuffer->BeginRenderPassEx(colors, resolves,
					depth ? retry->m_vulkan.m_imageView : nullptr, depth ? resolved->m_vulkan.m_imageView : nullptr,
					{ {}, { static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y) } }, 0, {}, true,
					VulkanRenderPassClearValues(glm::vec4(0x43 / 255.0f, 0x67 / 255.0f, 0xab / 255.0f, 1), 0.375f), true);
				driver.EndRenderPass(command);
				driver.RestoreImageBarriers(command);
				driver.EndCommandList(command);
				Require(driver.SubmitCommandList_Immediate(command), "a retried MSAA attachment must support real rendering and resolve");
				Require(ReadImage(driver, resolved->m_vulkan.m_image) ==
					std::vector<uint32_t>(extent.x * extent.y, depth ? 0x3ec00000u : 0xffab6743u),
					"every retried color/depth MSAA pixel must resolve to the expected value");
			}
		}
		std::cout << "MSAA cache: samples=" << device->GetCurrentMsaaSamples()
			<< "; color/depth refusal, retained pending/completed identity and 4806 resolved pixels passed\n";
	}

	enum class MsaaPassPath
	{
		Color, Depth, MixedColor, MixedDepth, Surface, SecondaryColor, SecondaryDepth, SecondarySurface
	};

	void TestMsaaPassRefusal()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		RenderingDispatchOverride recording(*device);
		for (auto path : { MsaaPassPath::Color, MsaaPassPath::Depth, MsaaPassPath::MixedColor, MsaaPassPath::MixedDepth,
			MsaaPassPath::Surface, MsaaPassPath::SecondaryColor, MsaaPassPath::SecondaryDepth, MsaaPassPath::SecondarySurface })
		{
			for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				const glm::ivec2 extent(61 + static_cast<uint32_t>(path) * 2 + (error == VK_ERROR_OUT_OF_DEVICE_MEMORY), 29);
				const glm::ivec4 area(0, 0, extent.x, extent.y);
				const bool depthOnly = path == MsaaPassPath::Depth || path == MsaaPassPath::MixedDepth || path == MsaaPassPath::SecondaryDepth;
				const bool surface = path == MsaaPassPath::Surface || path == MsaaPassPath::SecondarySurface;
				const bool secondary = path == MsaaPassPath::SecondaryColor || path == MsaaPassPath::SecondaryDepth || path == MsaaPassPath::SecondarySurface;
				auto setup = driver.CreateCommandList(false, ECommandListQueue::Graphics);
				driver.BeginCommandList(setup, true);
				auto color = driver.CreateRenderTarget(setup, extent, 1, ETextureFormat::R8G8B8A8_UNORM);
				auto depth = driver.CreateRenderTarget(setup, extent, 1, ETextureFormat::D32_SFLOAT,
					ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit |
					ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit);
				driver.ImageMemoryBarrier(setup, depth, EImageLayout::DepthAttachmentOptimal);
				RHISurfacePtr colorSurface;
				if (surface)
				{
					auto target = driver.GetOrAddMsaaFramebufferRenderTarget(color->GetFormat(), extent).StaticCast<RHIRenderTarget>();
					colorSurface = RHISurfacePtr::Make(target, color, true);
				}
				driver.RestoreImageBarriers(setup);
				driver.EndCommandList(setup);
				Require(driver.SubmitCommandList_Immediate(setup), "MSAA pass fixture inputs must finish initialization");
				TVector<RHITexturePtr> colors = depthOnly ? TVector<RHITexturePtr>{} : TVector<RHITexturePtr>{color};
				TVector<RHITexturePtr> resolves(colors.Num());
				RHIRenderTargetPtr extraColor;
				if (path == MsaaPassPath::MixedColor)
				{
					auto target = driver.GetOrAddMsaaFramebufferRenderTarget(color->GetFormat(), extent, 1).StaticCast<RHIRenderTarget>();
					extraColor = driver.CreateRenderTarget(extent, 1, color->GetFormat());
					colors.Add(target);
					resolves.Add(extraColor);
				}
				RHITexturePtr depthInput = depthOnly || surface ? RHITexturePtr(depth) : RHITexturePtr{};
				const glm::vec4 clearColor(0x43 / 255.0f, 0x67 / 255.0f, 0xab / 255.0f, 1);
				TVector<RHICommandListPtr> secondaryCommands;
				if (secondary)
				{
					auto command = driver.CreateCommandList(true, ECommandListQueue::Graphics);
					command->m_vulkan.m_commandBuffer->BeginSecondaryCommandList(
						depthOnly ? TVector<VkFormat>{} : TVector<VkFormat>{ VK_FORMAT_R8G8B8A8_UNORM },
						depthInput ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED,
						VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT);
					driver.ClearAttachments(command, area, glm::vec4(0x56 / 255.0f, 0x78 / 255.0f, 0x9a / 255.0f, 1), 0.625f);
					driver.EndCommandList(command);
					secondaryCommands.Add(command);
				}
				const auto record = [&](RHICommandListPtr command)
					{
						if (path == MsaaPassPath::Surface) return driver.BeginRenderPass(command, TVector<RHISurfacePtr>{colorSurface}, depth,
							area, glm::ivec2(0), true, clearColor, 0.375f, true);
						if (path == MsaaPassPath::SecondarySurface) return driver.RenderSecondaryCommandBuffers(command, secondaryCommands, TVector<RHISurfacePtr>{colorSurface}, depth,
							area, glm::ivec2(0), true, clearColor, 0.375f, true);
						if (secondary) return driver.RenderSecondaryCommandBuffers(command, secondaryCommands, colors, depthInput,
							area, glm::ivec2(0), true, clearColor, 0.375f, true, true);
						if (path == MsaaPassPath::MixedColor || path == MsaaPassPath::MixedDepth) return driver.BeginRenderPass(command, colors,
							resolves, depthInput, nullptr, area, glm::ivec2(0), true, clearColor, 0.375f, true, true);
						return driver.BeginRenderPass(command, colors, depthInput, area, glm::ivec2(0), true, clearColor, 0.375f, true, true);
					};
				auto command = driver.CreateCommandList(false, ECommandListQueue::Graphics);
				driver.BeginCommandList(command, true);
				const auto begins = nativePassBegins, ends = nativePassEnds;
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
					const auto submissions = submitCalls;
					Require(!record(command) && submitCalls == submissions + 1,
						"the actual MSAA pass overload must propagate its initialization refusal");
				}
				Require(nativePassBegins == begins && nativePassEnds == ends && command->GetNumRecordedCommands() == 0,
					"an unavailable MSAA attachment must record no native begin, unmatched end or secondary execution");
				Require(record(command), "the same MSAA pass inputs and command must succeed after refusal is removed");
				if (!secondary) driver.EndRenderPass(command);
				Require(nativePassBegins == begins + 1 && nativePassEnds == ends + 1,
					"the retry must record exactly one complete native pass");
				driver.RestoreImageBarriers(command);
				driver.EndCommandList(command);
				Require(driver.SubmitCommandList_Immediate(command), "the retried MSAA pass must execute");
				if (!depthOnly) Require(ReadImage(driver, color->m_vulkan.m_image) ==
					std::vector<uint32_t>(extent.x * extent.y, secondary ? 0xff9a7856u : 0xffab6743u),
					"the retried MSAA color pass must preserve every resolved pixel");
				if (extraColor) Require(ReadImage(driver, extraColor->m_vulkan.m_image) == std::vector<uint32_t>(extent.x * extent.y, 0xffab6743u),
					"mixed MRT retry must retain and resolve the supplied MSAA attachment too");
				if (depthInput) Require(ReadImage(driver, depth->m_vulkan.m_image) ==
					std::vector<uint32_t>(extent.x * extent.y, secondary ? 0x3f200000u : 0x3ec00000u),
					"the retried MSAA depth pass must preserve every resolved pixel");
			}
		}
		std::cout << "MSAA passes: samples=" << device->GetCurrentMsaaSamples()
			<< "; 16 primary/mixed/Surface/secondary refusals, balanced same-command retries and full color/depth pixels passed\n";
	}

	void TestMsaaDepthConsumers()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		for (bool highZ : { false, true })
		{
			const glm::ivec2 extent(highZ ? 113 : 111, 19);
			auto graph = RHIFrameGraphPtr::Make();
			auto depth = driver.CreateRenderTarget(extent, 1, ETextureFormat::D32_SFLOAT,
				ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit |
				ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit);
			auto pyramid = driver.CreateRenderTarget(extent, 1, ETextureFormat::R32_SFLOAT);
			graph->SetRenderTarget("DepthBuffer"_h, depth);
			Framegraph::FrameGraphNodePtr node = highZ ? Framegraph::FrameGraphNodePtr(TRefPtr<Framegraph::DepthHighZNode>::Make()) :
				Framegraph::FrameGraphNodePtr(TRefPtr<Framegraph::ClearNode>::Make());
			node->SetRHIResource(highZ ? "src"_h : "target"_h, depth);
			node->SetRHIResource("dst"_h, pyramid);
			node->SetFloat("clearDepth"_h, 0.375f);
			auto setup = driver.CreateCommandList(false, ECommandListQueue::Graphics);
			driver.BeginCommandList(setup, true);
			driver.ImageMemoryBarrier(setup, depth, EImageLayout::TransferDstOptimal);
			driver.ClearDepthStencil(setup, depth, 0.75f, 0);
			driver.ImageMemoryBarrier(setup, pyramid, EImageLayout::TransferDstOptimal);
			driver.ClearImage(setup, pyramid, glm::vec4(-8));
			driver.RestoreImageBarriers(setup);
			driver.EndCommandList(setup);
			Require(driver.SubmitCommandList_Immediate(setup), "depth consumer fixture must initialize its sentinel pixels");
			auto draw = driver.CreateCommandList(false, ECommandListQueue::Graphics);
			driver.BeginCommandList(draw, true);
			{
				SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), VK_ERROR_OUT_OF_HOST_MEMORY);
				const auto before = submitCalls;
				node->Process(graph, {}, draw, {});
				Require(submitCalls == before + 1 && draw->GetNumRecordedCommands() == 0 && !graph->HasCurrentDepthPyramid(pyramid),
					"a missing MSAA depth attachment must not record a clear, dispatch, or current pyramid");
			}
			driver.EndCommandList(draw);
			Require(driver.SubmitCommandList_Immediate(draw), "the refused node must leave a valid empty command list");
			Require(ReadImage(driver, depth->m_vulkan.m_image) == std::vector<uint32_t>(extent.x * extent.y, 0x3f400000u) &&
				ReadImage(driver, pyramid->m_vulkan.m_image) == std::vector<uint32_t>(extent.x * extent.y, 0xc1000000u),
				"refused Clear and HiZ nodes must preserve every sentinel depth and pyramid pixel");
		}
		std::cout << "MSAA depth consumers: refused Clear/HiZ preserve pixels, command recording and pyramid publication passed\n";
	}

	void TestSurfacePending()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver();
		for (bool targetFirst : { false, true })
		{
			if (targetFirst && device->GetCurrentMsaaSamples() == VK_SAMPLE_COUNT_1_BIT) continue;
			RHISurfacePtr surface;
			{
				FenceDispatchOverride completion(*device);
				SubmitOverride accepted(VulkanSubmissionTestAccess::UploadQueue(*device), VK_SUCCESS);
				auto resolved = driver.CreateRenderTarget(glm::ivec2(29, 21), 1, ETextureFormat::R8G8B8A8_UNORM);
				observedFences[0] = lastSubmittedFence;
				fenceResults[0] = VK_NOT_READY;
				surface = driver.CreateSurface(resolved);
				if (surface->NeedsResolve())
				{
					observedFences[1] = lastSubmittedFence;
					fenceResults[1] = VK_NOT_READY;
				}
				driver.TrackResources_ThreadSafe();
				std::cout << "Surface pending: samples=" << device->GetCurrentMsaaSamples()
					<< ", resolvedReady=" << resolved->IsReady() << ", surfaceReady=" << surface->IsReady() << '\n';
				Require(!resolved->IsReady() && !surface->IsReady(), "a Surface must not report ready while a child attachment is pending");
				for (size_t step = 0; step < observedFences.size(); ++step)
				{
					const size_t i = targetFirst ? 1 - step : step;
					if (!observedFences[i]) continue;
					Require(forwardFenceWait(*device, 1, &observedFences[i], VK_TRUE, 5000000000ull) == VK_SUCCESS,
						"accepted Surface initialization must complete on the native GPU");
					fenceResults[i] = VK_SUCCESS;
					driver.TrackResources_ThreadSafe();
					if (!step && surface->NeedsResolve())
						Require(!surface->IsReady() && surface->GetTarget()->IsReady() == targetFirst &&
							surface->GetResolved()->IsReady() != targetFirst,
							"completing either child alone must not make the Surface ready");
				}
				driver.TrackResources_ThreadSafe();
			}
			Require(surface->IsReady() && surface->GetTarget()->IsReady() && surface->GetResolved()->IsReady(),
				"a Surface must become ready when both accepted attachments complete");
		}
		std::cout << "Surface ownership: accepted pending children and completed readiness passed\n";
	}

	void CheckAttachmentPixels(RHIRenderTargetPtr resolved, RHISurfacePtr surface = {})
	{
		auto& driver = *Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		const auto extent = resolved->GetExtent();
		const bool depth = IsDepthFormat(resolved->GetFormat());
		auto command = driver.CreateCommandList(false, ECommandListQueue::Graphics);
		driver.BeginCommandList(command, true);
		if (surface && surface->NeedsResolve())
		{
			TVector<VulkanImageViewPtr> colors, resolves;
			if (!depth)
			{
				colors.Add(surface->GetTarget()->m_vulkan.m_imageView);
				resolves.Add(resolved->GetMipLayer(0)->m_vulkan.m_imageView);
			}
			command->m_vulkan.m_commandBuffer->BeginRenderPassEx(colors, resolves,
				depth ? surface->GetTarget()->m_vulkan.m_imageView : nullptr,
				depth ? resolved->GetMipLayer(0)->m_vulkan.m_imageView : nullptr,
				{ {}, { static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y) } }, 0, {}, true,
				VulkanRenderPassClearValues(glm::vec4(0x43 / 255.0f, 0x67 / 255.0f, 0xab / 255.0f, 1), 0.375f), true);
			driver.EndRenderPass(command);
		}
		for (uint32_t mip = surface && surface->NeedsResolve() ? 1u : 0u; mip < resolved->GetMipLevels(); ++mip)
		{
			auto target = resolved->GetMipLayer(mip);
			driver.ImageMemoryBarrier(command, target, EImageLayout::TransferDstOptimal);
			if (depth) driver.ClearDepthStencil(command, target, 0.375f, 0);
			else driver.ClearImage(command, target, glm::vec4(0x43 / 255.0f, 0x67 / 255.0f, 0xab / 255.0f, 1));
		}
		driver.RestoreImageBarriers(command);
		driver.EndCommandList(command);
		Require(driver.SubmitCommandList_Immediate(command), "retried attachments must execute actual GPU rendering");
		for (uint32_t mip = 0; mip < resolved->GetMipLevels(); ++mip)
		{
			const auto actual = ReadImage(driver, resolved->m_vulkan.m_image, mip);
			Require(std::all_of(actual.begin(), actual.end(), [&](uint32_t value) { return value == (depth ? 0x3ec00000u : 0xffab6743u); }),
				"every resolved pixel and mip of the retried attachment must match");
		}
	}

	void TestRenderTargetRefusal()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver();
		for (auto format : { ETextureFormat::R8G8B8A8_UNORM, ETextureFormat::D32_SFLOAT })
		{
			const auto usage = (IsDepthFormat(format) ? ETextureUsageBit::DepthStencilAttachment_Bit : ETextureUsageBit::ColorAttachment_Bit) |
				ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit;
			for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				const auto create = [&]() { return driver.CreateRenderTarget(glm::ivec2(23, 17), 2, format,
					ETextureFiltration::Nearest, ETextureClamping::Clamp, usage); };
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
					const auto before = nativeSubmitAttempts;
					auto rejected = create();
					Require(nativeSubmitAttempts == before + 1, "the fixture must refuse the target's actual initialization submission");
					std::cout << "Render target refusal: format=" << static_cast<uint32_t>(format) << ", error=" << error
						<< ", published=" << static_cast<bool>(rejected) << ", failed=" << (rejected && rejected->HasInitializationFailed()) << '\n';
					Require(!rejected, "a rejected render-target initialization must not publish a texture");
				}
				auto retry = create();
				Require(retry && retry->GetMipLevels() == 2 && retry->GetFiltration() == ETextureFiltration::Nearest &&
					retry->GetClamping() == ETextureClamping::Clamp, "a direct target retry must retain its requested properties");
				CheckAttachmentPixels(retry);
				const bool msaa = device->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
				for (uint32_t step = 0; step < (msaa ? 2u : 1u); ++step)
				{
					{
						SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error, false, step);
						const auto before = nativeSubmitAttempts;
						auto rejected = driver.CreateSurface(glm::ivec2(23, 17), 2, format,
							ETextureFiltration::Nearest, ETextureClamping::Clamp, usage);
						Require(!rejected && nativeSubmitAttempts == before + step + 1,
							"a Surface must stop at its first refused child without publishing partial state");
					}
					auto surface = driver.CreateSurface(glm::ivec2(23, 17), 2, format,
						ETextureFiltration::Nearest, ETextureClamping::Clamp, usage);
					Require(surface && surface->NeedsResolve() == msaa && surface->GetResolved()->GetMipLevels() == 2 &&
						(surface->GetTarget() == surface->GetResolved()) == !msaa, "Surface retry must preserve its resolve topology and mips");
					CheckAttachmentPixels(surface->GetResolved(), surface);
				}
				if (msaa)
				{
					{
						SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
						Require(!driver.CreateSurface(retry), "the retained-resolved overload must reject a failed MSAA child");
					}
					auto surface = driver.CreateSurface(retry);
					Require(surface && surface->GetResolved() == retry, "Surface retry must keep the caller's resolved identity");
					CheckAttachmentPixels(retry, surface);
				}
			}
		}
		std::cout << "Attachment factories: samples=" << device->GetCurrentMsaaSamples()
			<< "; color/depth OOM, direct and first/second Surface refusal, retained-resolved retries and all mip pixels passed\n";
	}

	void TestPreparedSurfaceRetry()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		if (device->GetCurrentMsaaSamples() == VK_SAMPLE_COUNT_1_BIT) return;
		auto& driver = *Renderer::GetDriver();
		auto graph = RHIFrameGraphPtr::Make();
		auto view = RHISceneViewPtr::Make();
		TVector<RHICommandListPtr> transfers, graphics;
		RHISemaphorePtr output;
		Require(graph->Process(view, transfers, graphics, {}, output), "the graph must prepare its shared fullscreen mesh before the cache fixture");
		auto node = TRefPtr<Framegraph::RenderSceneNode>::Make();
		auto color = driver.CreateRenderTarget(glm::ivec2(31, 19), 1, ETextureFormat::R8G8B8A8_UNORM);
		auto motion = driver.CreateRenderTarget(glm::ivec2(31, 19), 1, ETextureFormat::R8G8B8A8_UNORM);
		node->SetRHIResource("color"_h, color);
		node->SetRHIResource("motionVectors"_h, motion);
		graph->GetGraph().Add(node);
		auto input = driver.CreateWaitSemaphore();
		{
			SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), VK_ERROR_OUT_OF_HOST_MEMORY, false, 1);
			const auto before = nativeSubmitAttempts;
			Require(!graph->Process(view, transfers, graphics, input, output) && nativeSubmitAttempts == before + 2 &&
				transfers.IsEmpty() && graphics.IsEmpty() && output == input,
				"a partially prepared Surface set must reject the graph before consuming its semaphore or recording work");
		}
		auto first = graph->ResolveResource(color).DynamicCast<RHISurface>();
		Require(first && graph->ResolveResource(motion) == motion, "only an accepted prepared Surface may enter the graph cache");
		{
			SubmitOverride observe(VulkanSubmissionTestAccess::UploadQueue(*device), VK_SUCCESS);
			const auto before = nativeSubmitAttempts;
			Require(graph->Process(view, transfers, graphics, input, output) && nativeSubmitAttempts == before + 1,
				"unchanged static bindings must retry only the missing Surface");
		}
		auto second = graph->ResolveResource(motion).DynamicCast<RHISurface>();
		Require(second && graph->ResolveResource(color) == first, "retry must retain the previously accepted Surface identity");
		{
			SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), VK_ERROR_OUT_OF_HOST_MEMORY);
			const auto before = nativeSubmitAttempts;
			for (uint32_t repeat = 0; repeat < 16; ++repeat)
				Require(graph->Process(view, transfers, graphics, input, output) && graph->ResolveResource(color) == first &&
					graph->ResolveResource(motion) == second, "unchanged prepared Surfaces must remain reusable");
			Require(nativeSubmitAttempts == before, "warm static preparation must not submit any initialization work");
		}
		CheckAttachmentPixels(color, first);
		CheckAttachmentPixels(motion, second);
		std::cout << "Prepared Surfaces: partial refusal, retained owner, unchanged-binding retry, semaphore and 16 warm frames passed\n";
	}

	struct AttachmentGraphCase
	{
		FileId id;
		uint32_t rejectAt;
		uint32_t attempts;
		VkResult error;
	};

	std::vector<AttachmentGraphCase> WriteAttachmentGraphs(const std::filesystem::path& workspace, bool msaa)
	{
		std::vector<AttachmentGraphCase> cases;
		auto registry = App::GetSubmodule<AssetRegistry>();
		for (const std::string kind : { "texture", "surface", "prepared" })
		{
			const uint32_t submissions = msaa && kind != "texture" ? 4 : 2;
			for (uint32_t step = 0; step < submissions; ++step)
			{
				for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
				{
					auto description = YAML::Load(R"yaml(
renderTargets:
  - {name: Color, width: 23, height: 17, format: R8G8B8A8_UNORM}
  - {name: Motion, width: 23, height: 17, format: R8G8B8A8_UNORM}
frame: []
)yaml");
					if (kind == "surface")
						for (uint32_t target = 0; target < 2; ++target) description["renderTargets"][target]["bIsSurface"] = true;
					if (kind == "prepared")
						description["frame"].push_back(YAML::Load("{name: RenderScene, renderTargets: [{color: Color}, {motionVectors: Motion}]}"));
					const auto path = workspace / "Content" / ("Attachment-" + std::to_string(cases.size()) + ".renderer");
					std::ofstream(path) << description;
					// Preparation visits the complete Surface set before reporting an incomplete graph.
					const uint32_t attempts = kind == "prepared" && step >= 2 ? submissions : step + 1;
					cases.push_back({ registry->GetOrLoadFile(path.string()), step, attempts, error });
				}
			}
		}
		return cases;
	}

	void TestImportedAttachmentRefusal(const std::vector<AttachmentGraphCase>& cases)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto importer = App::GetSubmodule<FrameGraphImporter>();
		FrameGraphPtr previous;
		for (const auto& test : cases)
		{
			for (bool retainPrevious : { false, true })
			{
				auto output = retainPrevious ? previous : FrameGraphPtr{};
				SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), test.error, false, test.rejectAt);
				const auto before = nativeSubmitAttempts;
				const bool loaded = importer->LoadFrameGraph_Immediate(test.id, output);
				Require(!loaded && output == (retainPrevious ? previous : FrameGraphPtr{}) &&
					nativeSubmitAttempts == before + test.attempts,
					"an imported graph must reject every failed attachment stage without replacing caller state or entering its cache");
			}
			FrameGraphPtr repaired, cached;
			Require(importer->LoadFrameGraph_Immediate(test.id, repaired) && repaired, "the same graph asset must retry after attachment refusal");
			{
				SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), test.error);
				const auto before = nativeSubmitAttempts;
				Require(importer->LoadFrameGraph_Immediate(test.id, cached) && cached == repaired && nativeSubmitAttempts == before,
					"a successfully imported graph must retain its warm cache identity without resubmission");
			}
			auto graph = repaired->GetRHI();
			for (const auto name : { "Color"_h, "Motion"_h })
			{
				auto target = graph->GetRenderTarget(name);
				auto surface = graph->GetSurface(name);
				if (!surface) surface = graph->ResolveResource(target).DynamicCast<RHISurface>();
				CheckAttachmentPixels(target, surface);
			}
			previous = repaired;
		}
		std::cout << "Imported attachments: " << cases.size()
			<< " native fault stages, retained caller state, cache rejection/retry and complete two-target pixels passed\n";
	}

	void TestAttachmentTerminalLoss()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver();
		auto resolved = driver.CreateRenderTarget(glm::ivec2(11, 7), 1, ETextureFormat::R8G8B8A8_UNORM);
		CheckAttachmentPixels(resolved);
		SubmitOverride loss(VulkanSubmissionTestAccess::UploadQueue(*device), VK_ERROR_DEVICE_LOST);
		const auto before = nativeSubmitAttempts;
		Require(!driver.CreateSurface(glm::ivec2(13, 7), 1, ETextureFormat::R8G8B8A8_UNORM) &&
			device->IsDeviceLost() && nativeSubmitAttempts == before + 1,
			"terminal loss must stop Surface creation at its refused resolved attachment");
		Require(!driver.CreateSurface(resolved) &&
			!driver.CreateRenderTarget(glm::ivec2(13, 7), 1, ETextureFormat::R8G8B8A8_UNORM) && nativeSubmitAttempts == before + 1,
			"terminal loss must not submit more work or wrap an existing attachment as a new Surface");
		std::cout << "Attachment terminal loss: first refusal, existing-resolved rejection and no further submissions passed\n";
	}

	void TestParticleShadowInitialization();

	class LightingPublicationNode : public Framegraph::ClearNode
	{
	public:
		void Process(RHIFrameGraphPtr, RHICommandListPtr, RHICommandListPtr, const RHISceneViewSnapshot& scene) override
		{
			shadowMaps.Clear();
			if (scene.m_rhiLightsData) scene.m_rhiLightsData->GetShaderBindings().TryGet("shadowMaps"_h, shadowMaps);
			++frames;
		}

		RHIShaderBindingPtr shadowMaps;
		uint32_t frames = 0;
	};

	void TestLightingShadowInitialization()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto renderer = App::GetSubmodule<Renderer>();
		auto scheduler = App::GetSubmodule<Tasks::Scheduler>();
		Require(renderer->EnsureFrameGraph(), "the lighting fixture requires its empty renderer graph");
		auto observer = TRefPtr<LightingPublicationNode>::Make();
		renderer->GetFrameGraph()->GetRHI()->GetGraph().Add(observer);
		for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			auto world = App::GetSubmodule<EngineLoop>()->CreateEmptyWorld("Default shadow initialization",
				static_cast<uint8_t>(EWorldBehaviourBit::EcsTickable));
			Sailor::FrameState first(world.GetRawPtr(), 16, {}, { 32, 24 });
			{
				SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
				const auto before = nativeSubmitAttempts;
				world->Tick(first);
				Require(nativeSubmitAttempts == before + 1, "world startup must encounter the refused default shadow initialization");
			}
			auto push = [&](Sailor::FrameState& frame)
			{
				const auto before = observer->frames;
				Require(renderer->PushFrame(frame), "the real renderer must accept the lighting fixture frame");
				scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
				scheduler->ProcessTasksOnMainThread();
				Require(observer->frames == before + 1, "the lighting snapshot must reach the actual render graph");
			};
			{
				SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
				push(first);
				Require(!observer->shadowMaps, "failed default shadow initialization must not publish null sampler elements");
			}
			Sailor::FrameState retry(world.GetRawPtr(), 32, {}, { 32, 24 });
			world->Tick(retry);
			push(retry);
			Require(observer->shadowMaps && observer->shadowMaps->GetTextureBindings().Num() == LightingECS::MaxShadowMapSamplers,
				"the unchanged world must publish all default shadow samplers on retry");
			auto target = observer->shadowMaps->GetTextureBinding();
			Require(target && target->GetExtent() == glm::ivec2(1) && target->GetFormat() == LightingECS::ShadowMapFormat,
				"the recovered shadow fallback must have its original extent and format");
			for (const auto& texture : observer->shadowMaps->GetTextureBindings())
				Require(texture == target, "every unused shadow slot must retain the accepted fallback");
			Sailor::FrameState warm(world.GetRawPtr(), 48, {}, { 32, 24 });
			world->Tick(warm);
			{
				SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
				const auto before = nativeSubmitAttempts;
				push(warm);
				Require(nativeSubmitAttempts == before && observer->shadowMaps->GetTextureBinding() == target,
					"warm lighting must reuse its accepted default shadow without initialization submissions");
			}
		}
		std::cout << "Lighting fallback: world startup refusal, absent publication, frame retry and all 128 warm shadow samplers passed\n";
	}

	int RunAttachmentGpu(int argc, const char** argv, bool cachedMsaa)
	{
		Tests::TempDirectory workspace(cachedMsaa ? "msaa-cache" : "render-targets");
		int result = 1;
		try
		{
			std::string enginePath = std::filesystem::current_path().string();
			for (int i = 1; i + 1 < argc; ++i)
				if (std::string_view(argv[i]) == "--workspace") enginePath = argv[i + 1];
			std::filesystem::create_directory(workspace.Path("Content"));
			if (!cachedMsaa) std::ofstream(workspace.Path("Content/EditorRenderer.renderer")) << "renderTargets: []\nframe: []\n";
			YAML::Node manifest;
			manifest["manifestVersion"] = 1;
			manifest["workspaceId"] = "00000000-0000-0000-0000-000000000223";
			manifest["name"] = "MSAA cache test";
			manifest["enginePath"] = enginePath;
			manifest["engineReferenceKind"] = "source";
			manifest["contentPath"] = "Content";
			manifest["sourcePath"] = "Source";
			manifest["generatedProjectPath"] = "Generated";
			manifest["cachePath"] = "Cache";
			manifest["buildPath"] = "Cache/Build";
			manifest["logicOutputPath"] = "Binaries";
			manifest["logicModuleName"] = "MsaaCacheTest";
			std::ofstream(workspace.Path("workspace.sailor")) << manifest;
			auto settings = YAML::LoadFile((std::filesystem::path(enginePath) / "ProjectSettings.yaml").string());
			settings["graphics"]["defaultQuality"] = "High";
			const std::string root = workspace.Get().string();
			std::vector<const char*> arguments(argv, argv + argc);
			arguments.insert(arguments.end(), { "--workspace", root.c_str(), "--world", "", "--editor", "--port", "0", "--new-world" });
			for (uint32_t samples : { 1u, 2u, 4u })
			{
				if (cachedMsaa && samples == 1) continue;
				for (const char* preset : { "Ultra", "High", "Medium", "Low", "VeryLow" })
					settings["graphics"]["presets"][preset]["msaaSamples"] = samples;
				std::ofstream(workspace.Path("ProjectSettings.yaml")) << settings;
				Require(App::Initialize(arguments.data(), static_cast<int32_t>(arguments.size())) == EAppInitializationResult::Ready,
					"the MSAA fixture must initialize a hidden native App");
				App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
				const auto graphs = cachedMsaa ? std::vector<AttachmentGraphCase>{} : WriteAttachmentGraphs(workspace.Get(), samples > 1);
				if (!cachedMsaa && samples == 1) TestLightingShadowInitialization();
				OnRender([&]()
					{
						Require(static_cast<uint32_t>(VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples()) == samples,
							"the native attachment fixture requires the requested sample count");
						if (!cachedMsaa)
						{
							TestSurfacePending();
							TestRenderTargetRefusal();
							TestPreparedSurfaceRetry();
							TestImportedAttachmentRefusal(graphs);
							if (samples == 1) TestParticleShadowInitialization();
							TestAttachmentTerminalLoss();
							return;
						}
						TestMsaaCacheRefusal();
						TestMsaaPassRefusal();
						TestMsaaDepthConsumers();
						auto device = VulkanApi::GetInstance()->GetMainDevice();
						auto& driver = *Renderer::GetDriver();
						const glm::ivec2 extent(43, 27);
						Require(driver.GetOrAddMsaaFramebufferRenderTarget(ETextureFormat::R8G8B8A8_UNORM, extent).IsValid(),
							"the loss fixture needs an accepted warm target");
						SubmitOverride loss(VulkanSubmissionTestAccess::UploadQueue(*device), VK_ERROR_DEVICE_LOST);
						const auto before = submitCalls;
						Require(!driver.GetOrAddMsaaFramebufferRenderTarget(ETextureFormat::R8G8B8A8_UNORM, glm::ivec2(91, 37)) &&
							device->IsDeviceLost() && submitCalls == before + 1,
							"a lost-device initialization must not publish its target");
						Require(!driver.GetOrAddMsaaFramebufferRenderTarget(ETextureFormat::R8G8B8A8_UNORM, extent) &&
							!driver.GetOrAddMsaaFramebufferRenderTarget(ETextureFormat::D32_SFLOAT, glm::ivec2(93, 37)) && submitCalls == before + 1,
							"terminal loss must reject warm and cold lookups without another native submission");
						std::cout << "MSAA terminal loss: warm/cold rejection without resubmission passed\n";
					});
				App::Stop();
				Require(App::Shutdown(), "each native MSAA configuration must release its App");
			}
			std::cout << (cachedMsaa ? "Native MSAA cache and render-pass failure tests passed at 2x and 4x\n" :
				"Native render-target and Surface factory tests passed at 1x, 2x and 4x\n");
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		if (!App::Shutdown()) result = 1;
		return result;
	}

	void TestAsynchronousImagePublication(bool cubemap)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		const auto usage = ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferSrc_Bit |
			ETextureUsageBit::TextureTransferDst_Bit;
		uint32_t cases = 0, pixelsRead = 0;
		for (auto type : { ETextureType::Texture1D, ETextureType::Texture2D, ETextureType::Texture3D, ETextureType::Cubemap })
		{
			if (cubemap && type != ETextureType::Cubemap) continue;
			const glm::ivec3 extent(8, type == ETextureType::Texture1D ? 1 : 8, type == ETextureType::Texture3D ? 8 : 1);
			const uint32_t layers = type == ETextureType::Cubemap ? 6 : 1;
			const size_t layerSize = size_t(extent.x) * extent.y * extent.z;
			std::vector<uint32_t> data(layerSize * layers);
			for (uint32_t face = 0; face < layers; ++face)
				std::fill_n(data.begin() + layerSize * face, layerSize, 0xff123420u + 17u * face);
			for (uint32_t levels : { 1u, 4u })
			for (bool upload : { false, true })
			{
				if (cubemap && upload) continue;
				const auto create = [&]() -> RHITexturePtr
				{
					if (cubemap) return driver.CreateCubemap(glm::ivec2(extent), levels, EFormat::R8G8B8A8_UNORM,
						ETextureFiltration::Nearest, ETextureClamping::Clamp, usage);
					return driver.CreateTexture(upload ? data.data() : nullptr, upload ? data.size() * sizeof(uint32_t) : 0,
						extent, levels, type, EFormat::R8G8B8A8_UNORM, ETextureFiltration::Nearest, ETextureClamping::Clamp, usage);
				};
				for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
				{
					{
						SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
						const auto before = nativeSubmitAttempts;
						auto rejected = create();
						std::cout << "Asynchronous image refusal: cubemap=" << cubemap << ", published=" << bool(rejected)
							<< ", failed=" << (rejected && rejected->HasInitializationFailed()) << '\n';
						Require(!rejected && nativeSubmitAttempts == before + 1,
							"a refused asynchronous image initialization must not publish its texture or cubemap");
					}
					RHITexturePtr accepted;
					{
						FenceDispatchOverride completion(*device);
						SubmitOverride submission(VulkanSubmissionTestAccess::UploadQueue(*device), VK_SUCCESS);
						accepted = create();
						Require(accepted.IsValid(), "the unchanged image request must retry successfully");
						observedFences[0] = lastSubmittedFence;
						fenceResults[0] = VK_NOT_READY;
						driver.TrackResources_ThreadSafe();
						Require(!accepted->IsReady() && !accepted->HasInitializationFailed(),
							"accepted pending image work must stay distinct from a refused initialization");
						Require(forwardFenceWait(*device, 1, &observedFences[0], VK_TRUE, 5000000000ull) == VK_SUCCESS,
							"accepted image initialization must finish on the native GPU");
						fenceResults[0] = VK_SUCCESS;
						driver.TrackResources_ThreadSafe();
					}
					Require(accepted->IsReady() && accepted->GetFiltration() == ETextureFiltration::Nearest &&
						accepted->GetClamping() == ETextureClamping::Clamp && accepted->HasMipMaps() == (levels > 1) &&
						accepted->GetFormat() == EFormat::R8G8B8A8_UNORM &&
						accepted->m_vulkan.m_image->m_mipLevels == levels && accepted->m_vulkan.m_image->m_arrayLayers == layers &&
						accepted->m_vulkan.m_image->m_extent.depth == uint32_t(extent.z),
						"retry must retain the requested image shape, sampling properties and mip/layer count");
					if (!upload)
					{
						auto command = driver.CreateCommandList(false, ECommandListQueue::Graphics);
						driver.BeginCommandList(command, true);
						driver.ImageMemoryBarrier(command, accepted, EImageLayout::TransferDstOptimal);
						driver.ClearImage(command, accepted, glm::vec4(0x43 / 255.0f, 0x67 / 255.0f, 0xab / 255.0f, 1));
						driver.RestoreImageBarriers(command);
						driver.EndCommandList(command);
						Require(driver.SubmitCommandList_Immediate(command), "the retried empty image must accept native commands");
					}
					for (uint32_t mip = 0; mip < levels; ++mip)
					for (uint32_t face = 0; face < layers; ++face)
					{
						const auto pixels = ReadImage(driver, accepted->m_vulkan.m_image, mip, face);
						const uint32_t expected = upload ? data[face * layerSize] : 0xffab6743u;
						Require(std::all_of(pixels.begin(), pixels.end(), [&](auto value) { return value == expected; }),
							"every retried image voxel, face and generated mip must contain the expected pixels");
						pixelsRead += static_cast<uint32_t>(pixels.size());
					}
					++cases;
				}
			}
		}
		std::cout << "Asynchronous image factories: cubemap=" << cubemap << ", " << cases
			<< " refused/retried cases, " << pixelsRead << " complete mip/face/volume pixels passed\n";
		{
			SubmitOverride loss(VulkanSubmissionTestAccess::UploadQueue(*device), VK_ERROR_DEVICE_LOST);
			const auto before = nativeSubmitAttempts;
			RHITexturePtr rejected = cubemap ? driver.CreateCubemap(glm::ivec2(8), 1, EFormat::R8G8B8A8_UNORM) :
				driver.CreateTexture(nullptr, 0, glm::ivec3(8, 8, 1), 1, ETextureType::Texture2D, EFormat::R8G8B8A8_UNORM);
			Require(!rejected && device->IsDeviceLost() && nativeSubmitAttempts == before + 1,
				"terminal initialization loss must not publish an asynchronous image");
			Require(!driver.CreateCubemap(glm::ivec2(8), 1, EFormat::R8G8B8A8_UNORM) &&
				!driver.CreateTexture(nullptr, 0, glm::ivec3(8, 8, 1), 1, ETextureType::Texture2D, EFormat::R8G8B8A8_UNORM) &&
				nativeSubmitAttempts == before + 1,
				"a lost device must reject both image factories without another native submission");
		}
		std::cout << "Asynchronous image terminal loss: first refusal and both factory short-circuits passed\n";
	}

	void TestCubemapPending()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver();
		RHICubemapPtr cube;
		{
			FenceDispatchOverride completion(*device);
			SubmitOverride accepted(VulkanSubmissionTestAccess::UploadQueue(*device), VK_SUCCESS);
			cube = driver.CreateCubemap(glm::ivec2(8), 4u, EFormat::R8G8B8A8_UNORM);
			observedFences[0] = lastSubmittedFence;
			fenceResults[0] = VK_NOT_READY;
			driver.TrackResources_ThreadSafe();
			std::cout << "Cubemap pending: parent=" << cube->IsReady() << ", face=" << cube->GetFace(0)->IsReady()
				<< ", mip=" << cube->GetMipLevel(1)->IsReady() << '\n';
			Require(!cube->IsReady(), "the accepted cubemap must stay pending while initialization is held");
			for (uint32_t mip = 0; mip < 4; ++mip)
			{
				if (mip) Require(!cube->GetMipLevel(mip)->IsReady(), "pending cubemap mip views must not report ready");
				for (uint32_t face = 0; face < 6; ++face)
					Require(!cube->GetFace(face, mip)->IsReady(), "pending cubemap face views must not report ready");
			}
			Require(forwardFenceWait(*device, 1, &observedFences[0], VK_TRUE, 5000000000ull) == VK_SUCCESS,
				"accepted cubemap initialization must finish on the native GPU");
			fenceResults[0] = VK_SUCCESS;
			driver.TrackResources_ThreadSafe();
		}
		Require(cube->IsReady(), "the completed cubemap must become ready");
		for (uint32_t mip = 0; mip < 4; ++mip)
		{
			if (mip) Require(cube->GetMipLevel(mip)->IsReady(), "completed cubemap mip views must become ready");
			for (uint32_t face = 0; face < 6; ++face)
				Require(cube->GetFace(face, mip)->IsReady(), "completed cubemap face views must become ready");
		}
		std::cout << "Cubemap ownership: accepted pending parent, all face/mip views and native completion passed\n";
	}

	class ImGuiFontProbe : public ImGuiApi
	{
	public:
		static auto Backend() { return ImGui_GetBackendData(); }
		static void Reinitialize()
		{
			auto info = ImGui_GetBackendData()->InitInfo;
			ImGui_Shutdown();
			ImGui::GetIO().Fonts->SetTexID(0);
			Require(ImGui_Init(&info), "the renderer backend must initialize even while font upload is pending");
		}
	};

	void TestImGuiFontInitialization()
	{
		ImGui::SetCurrentContext(ImGuiApi::GetCurrentContext());
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver();
		auto* engine = App::GetSubmodule<EngineLoop>();
		{
			Sailor::FrameState frame(engine->GetWorld().GetRawPtr(), 16, {}, { 32, 24 });
			engine->ProcessCpuFrame(frame);
			frame.GetDrawImGuiTask()->Wait();
			driver.WaitIdle();
			driver.TrackResources_ThreadSafe();
		}
		for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			{
				SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
				const auto before = nativeSubmitAttempts;
				ImGuiFontProbe::Reinitialize();
				Sailor::FrameState frame(engine->GetWorld().GetRawPtr(), 32, {}, { 32, 24 });
				engine->ProcessCpuFrame(frame);
				frame.GetDrawImGuiTask()->Wait();
				Require(nativeSubmitAttempts == before + 2 && !ImGuiFontProbe::Backend()->FontTexture &&
					ImGui::GetIO().Fonts->TexID == 0 && frame.GetDrawImGuiTask()->GetResult()->GetNumRecordedCommands() == 0,
					"refused startup and next-frame font uploads must publish no texture ID or UI draw commands");
			}
			Sailor::FrameState retry(engine->GetWorld().GetRawPtr(), 48, {}, { 32, 24 });
			engine->ProcessCpuFrame(retry);
			retry.GetDrawImGuiTask()->Wait();
			driver.WaitIdle();
			driver.TrackResources_ThreadSafe();
			const auto font = ImGuiFontProbe::Backend()->FontTexture;
			Require(font && font->IsReady() && ImGui::GetIO().Fonts->TexID == reinterpret_cast<ImTextureID>(font.GetRawPtr()),
				"the unchanged backend must publish a completed font texture on the next CPU frame");
			{
				SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
				const auto before = nativeSubmitAttempts;
				Sailor::FrameState warm(engine->GetWorld().GetRawPtr(), 64, {}, { 32, 24 });
				engine->ProcessCpuFrame(warm);
				warm.GetDrawImGuiTask()->Wait();
				Require(nativeSubmitAttempts == before && ImGuiFontProbe::Backend()->FontTexture == font,
					"warm CPU frames must reuse the accepted font image without initialization uploads");
			}
		}
		std::cout << "ImGui font initialization: startup/frame refusal, no rejected UI draws, next-frame recovery and warm reuse passed\n";
	}

	void TestMeshInitialization()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		auto& driver = *Renderer::GetDriver();
		const std::array<glm::vec2, 4> vertices{ glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1) };
		const std::array<uint32_t, 6> indices{ 0, 1, 2, 0, 2, 3 };
		const auto create = [&](bool generic)
		{
			auto task = Tasks::CreateTaskWithResult<RHIMeshPtr>("Native mesh upload"_h, [&, generic]()
				{
					auto mesh = driver.CreateMesh();
					mesh->m_vertexDescription = RHIVertexDescriptionPtr::Make();
					mesh->m_vertexDescription->SetVertexStride(sizeof(glm::vec2));
					mesh->m_vertexDescription->AddAttribute(0, 0, EFormat::R32G32_SFLOAT, 0);
					if (generic) driver.IGraphicsDriver::UpdateMesh(mesh, vertices.data(), sizeof(vertices), indices.data(), sizeof(indices));
					else driver.UpdateMesh(mesh, vertices.data(), sizeof(vertices), indices.data(), sizeof(indices));
					return mesh;
				}, EThreadType::RHI);
			task->Run();
			task->Wait();
			return task->GetResult();
		};
		using BufferOwner = Memory::TManagedMemory<Memory::VulkanBufferMemoryPtr, VulkanBufferAllocator>;
		for (bool generic : { false, true })
		{
			for (const auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				RHIMeshPtr failed;
				{
					WorkerUploadOverride refusal(error, true);
					failed = create(generic);
					Require(workerSubmitRefusals.load() == 1 && workerSubmitWasOffCaller.load() &&
						failed->HasInitializationFailed() && !failed->IsReady(),
						"mesh upload must expose native rejection before its RHI task returns");
				}
				TWeakPtr<BufferOwner> failedVertices(failed->m_vertexBuffer->m_vulkan.m_buffer);
				TWeakPtr<BufferOwner> failedIndices(failed->m_indexBuffer->m_vulkan.m_buffer);
				RHIMeshPtr accepted;
				{
					FenceDispatchOverride dispatch(*device);
					WorkerUploadOverride observe(VK_SUCCESS, true);
					accepted = create(generic);
					Require(workerSubmitCalls.load() == 1 && workerSubmittedFence.load(), "mesh retry must submit once");
					observedFences[0] = workerSubmittedFence.load();
					fenceResults[0] = VK_NOT_READY;
					driver.TrackResources_ThreadSafe();
					Require(!accepted->IsReady() && !accepted->HasInitializationFailed(), "accepted mesh upload must remain pending");
				}
				Require(device->WaitIdle() == VK_SUCCESS, "accepted mesh upload must complete");
				driver.TrackResources_ThreadSafe();
				Require(accepted->IsReady() && !failed->IsReady(), "completion must not resurrect a refused mesh");
				OnRender([&]()
					{
						const auto memory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
						auto vertexReadback = driver.CreateBuffer(sizeof(vertices), EBufferUsageBit::BufferTransferDst_Bit, memory);
						auto indexReadback = driver.CreateBuffer(sizeof(indices), EBufferUsageBit::BufferTransferDst_Bit, memory);
						Require(driver.CopyBuffer_Immediate(accepted->m_vertexBuffer, vertexReadback, sizeof(vertices)) &&
							driver.CopyBuffer_Immediate(accepted->m_indexBuffer, indexReadback, sizeof(indices)), "mesh readback must complete");
						Require(std::memcmp(vertexReadback->GetPointer(), vertices.data(), sizeof(vertices)) == 0 &&
							std::memcmp(indexReadback->GetPointer(), indices.data(), sizeof(indices)) == 0,
							"all retried mesh vertices and indices must match their source");
					});
				failed.Clear();
				scheduler->WaitIdle({ EThreadType::RHI, EThreadType::Render });
				OnRender([&]() { driver.CollectGarbage_RenderThread(); });
				Require(!failedVertices.TryLock() && !failedIndices.TryLock(), "failed mesh/fence ownership must release both allocations");
			}
		}
		std::cout << "Mesh initialization: both implementations, four native refusals, pending retries, 224 vertex/index bytes and failed allocation release passed\n";
		{
			WorkerUploadOverride loss(VK_ERROR_DEVICE_LOST, true);
			auto failed = create(false);
			Require(workerSubmitRefusals.load() == 1 && device->IsDeviceLost() && failed->HasInitializationFailed() && !failed->IsReady(),
				"mesh submission loss must remain terminal");
			for (bool generic : { false, true })
			{
				auto rejected = create(generic);
				Require(rejected->HasInitializationFailed() && !rejected->IsReady() && workerSubmitCalls.load() == 1,
					"neither mesh implementation may submit or become ready after terminal loss");
			}
		}
		std::cout << "Mesh initialization: native terminal loss and both failed-owner short-circuits passed\n";
	}

	void TestImmediateImageContents()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver();
		std::array<uint32_t, 16> data{};
		for (uint32_t i = 0; i < data.size(); ++i) data[i] = 0xff102030u + 137u * i;
		auto create = [&]() { return driver.CreateImage_Immediate(data.data(), sizeof(data), glm::ivec3(4, 4, 1), 1u,
			ETextureType::Texture2D, ETextureFormat::R8G8B8A8_UNORM, ETextureFiltration::Nearest, ETextureClamping::Repeat); };
		auto verify = [&]()
			{
				auto image = create();
				Require(image && image->IsReady() && image->m_vulkan.m_imageView, "completed upload must publish a ready texture and view");
				Require(image->GetFiltration() == ETextureFiltration::Nearest && image->GetClamping() == ETextureClamping::Repeat &&
					image->GetFormat() == ETextureFormat::R8G8B8A8_UNORM && !image->HasMipMaps() &&
					image->GetDefaultLayout() == EImageLayout::ShaderReadOnlyOptimal, "immediate upload changed texture sampling properties");
				const auto readback = ReadImage(driver, image->m_vulkan.m_image);
				Require(std::equal(data.begin(), data.end(), readback.begin()), "2D upload changed source pixels");
			};
		verify();
		for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			{
				SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
				Require(!create(), "refused image upload must not publish a texture");
			}
			verify();
			{
				FenceDispatchOverride dispatch(*device);
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				fenceWaitResult = error;
				captureNextFenceWait = true;
				capturedFenceCompleted = false;
				Require(!create() && capturedFenceCompleted, "image upload must propagate accepted wait failure");
				driver.TrackResources_ThreadSafe();
				fenceResults[0] = VK_SUCCESS;
				driver.TrackResources_ThreadSafe();
			}
			verify();
		}

		std::array<uint32_t, 96> faces{};
		for (uint32_t face = 0; face < 6u; ++face)
			std::fill_n(faces.begin() + 16u * face, 16u, 0xff123420u + 17u * face);
		auto cubemap = driver.CreateImage_Immediate(faces.data(), sizeof(faces), glm::ivec3(4, 4, 1), 3u,
			ETextureType::Cubemap, ETextureFormat::R8G8B8A8_UNORM);
		Require(cubemap && cubemap->m_vulkan.m_image->m_arrayLayers == 6u && cubemap->m_vulkan.m_image->m_mipLevels == 3u &&
			cubemap->HasMipMaps() && cubemap->m_vulkan.m_imageView->m_viewType == VK_IMAGE_VIEW_TYPE_CUBE,
			"cubemap upload lost its layers, mip chain or cube view");
		for (uint32_t mip = 0; mip < 3u; ++mip)
		{
			for (uint32_t face = 0; face < 6u; ++face)
			{
				const auto readback = ReadImage(driver, cubemap->m_vulkan.m_image, mip, face);
				Require(std::all_of(readback.begin(), readback.end(), [&](uint32_t value) { return value == faces[16u * face]; }),
					"cubemap face or generated mip contains incorrect pixels");
			}
		}
		auto volume = driver.CreateImage_Immediate(data.data(), 8u * sizeof(uint32_t), glm::ivec3(2), 1u,
			ETextureType::Texture3D, ETextureFormat::R8G8B8A8_UNORM);
		Require(volume && volume->m_vulkan.m_imageView->m_viewType == VK_IMAGE_VIEW_TYPE_3D, "volume upload lost its 3D view");
		const auto volumeData = ReadImage(driver, volume->m_vulkan.m_image);
		Require(std::equal(data.begin(), data.begin() + 8u, volumeData.begin()), "volume upload changed source voxels");
	}

	class BootstrapDriver final : public VulkanGraphicsDriver
	{
	public:
		BootstrapDriver(uint32_t failureCall, VkResult error, bool waitFailure) :
			m_failureCall(failureCall), m_error(error), m_waitFailure(waitFailure) {}

		RHITexturePtr CreateImage_Immediate(const void* data, size_t size, glm::ivec3 extent, uint32_t mipLevels,
			ETextureType type, ETextureFormat format, ETextureFiltration filtration,
			ETextureClamping clamping, ETextureUsageFlags usage) override
		{
			if (++imageCalls == m_failureCall)
			{
				auto device = VulkanApi::GetInstance()->GetMainDevice();
				if (m_waitFailure)
				{
					m_wait = TUniquePtr<FenceDispatchOverride>::Make(*device);
					fenceResults = { VK_NOT_READY, VK_NOT_READY };
					fenceWaitResult = m_error;
					captureNextFenceWait = true;
					capturedFenceCompleted = false;
				}
				else m_submit = TUniquePtr<SubmitOverride>::Make(VulkanSubmissionTestAccess::UploadQueue(*device), m_error);
			}
			return VulkanGraphicsDriver::CreateImage_Immediate(data, size, extent, mipLevels, type, format, filtration, clamping, usage);
		}

		bool SubmitCommandList(RHICommandListPtr command, RHIFencePtr fence, RHISemaphorePtr signal, RHISemaphorePtr wait) override
		{
			lastCommand = command;
			return VulkanGraphicsDriver::SubmitCommandList(command, fence, signal, wait);
		}

		bool HasAnyDefault() const { return m_defaultTexture || m_vkDefaultTexture || m_vkDefaultCubemap; }
		VulkanImagePtr DefaultCubemap() const { return m_vkDefaultCubemap ? m_vkDefaultCubemap->GetImage() : nullptr; }
		size_t PendingCount() const { return m_trackedFences.Num(); }
		void ResetFailure() { m_wait.Clear(); m_submit.Clear(); }

		uint32_t imageCalls = 0u;
		RHICommandListPtr lastCommand;

	private:
		uint32_t m_failureCall;
		VkResult m_error;
		bool m_waitFailure;
		TUniquePtr<FenceDispatchOverride> m_wait;
		TUniquePtr<SubmitOverride> m_submit;
	};

	int RunBootstrapGpu(int argc, const char** argv, bool waitFailure, bool lost, bool capabilityTests = false)
	{
		int result = 1;
		try
		{
			// Stop App bootstrap before it creates a renderer; the driver still gets
			// its normal window and scheduler dependencies, with no Renderer singleton.
			const auto missingManifest = (std::filesystem::temp_directory_path() / "sailor-bootstrap-missing-manifest.yaml").string();
			Require(!std::filesystem::exists(missingManifest), "bootstrap fixture needs an absent manifest");
			std::vector<const char*> arguments(argv, argv + argc);
			arguments.insert(arguments.end(), { "--workspace-manifest", missingManifest.c_str() });
			App::Initialize(arguments.data(), static_cast<int32_t>(arguments.size()));
			Require(App::GetInstance() && !App::GetSubmodule<Renderer>() && !VulkanApi::GetInstance(),
				"bootstrap fixture must start without an existing renderer or Vulkan instance");
			App::AddSubmodule(TSubmodule<Tasks::Scheduler>::Make())->Initialize();
			auto& window = App::GetMainWindow();
			window = TUniquePtr<Win32::Window>::Make();
			window->Create("VulkanBootstrapTests", "VulkanBootstrapTests", 320, 240, false, false, 0);
			window->Show(false);

#if defined(__APPLE__)
			if (capabilityTests)
			{
				auto& overrides = Sailor::Tests::GetVulkanCapabilityOverrides();
				using Layers = Sailor::Tests::ValidationLayerInventory;
				for (auto inventory : { Layers::None, Layers::Primary, Layers::Compatibility, Layers::Both })
				{
					for (bool debug : { false, true })
					{
						const uint32_t enumerations = overrides.layerEnumerationCalls, creates = overrides.instanceCreateCalls;
						const uint32_t devices = overrides.deviceCreateCalls;
						overrides.validationLayers = inventory;
						BootstrapDriver driver(0u, VK_SUCCESS, false);
						driver.Initialize(window.GetRawPtr(), EMsaaSamples::Samples_1, debug);
						overrides.validationLayers = Layers::Native;
						const bool expected = debug && (inventory == Layers::Primary || inventory == Layers::Both);
						Require(overrides.instanceCreateCalls == creates + 1u && (overrides.layerEnumerationCalls > enumerations) == debug,
							"validation fixture must observe actual layer enumeration and instance configuration");
						Require(overrides.requestedPrimaryValidation == expected && overrides.requestedLayerCount == (expected ? 1u : 0u) &&
							!overrides.requestedCompatibilityLayer,
							"an installed primary validation layer must not depend on the synchronization2 compatibility layer");
						Require(overrides.requestedDebugMessenger == expected, "the debug callback must follow the selected validation configuration");
						Require(!driver.IsInitialized() && !VulkanApi::GetInstance() && overrides.deviceCreateCalls == devices && driver.imageCalls == 0,
							"fake layer configuration must stop before creating a native instance or device");
					}
				}
				std::cout << "Vulkan validation selection: debug off/on, no/primary/compatibility/both inventories and debug callbacks passed\n";
				const auto expectFailure = [&](uint32_t deviceCreates, uint32_t deviceDestroys, uint32_t instances = 1u)
				{
					const uint32_t creates = overrides.deviceCreateCalls, destroys = overrides.deviceDestroyCalls;
					const uint32_t instanceCreates = overrides.instanceCreateCalls, instanceDestroys = overrides.instanceDestroyCalls;
					const uint32_t surfaces = overrides.surfaceDestroyCalls;
					const uint32_t samplers = overrides.samplerCreateCalls, buffers = overrides.bufferCreateCalls;
					BootstrapDriver driver(0u, VK_SUCCESS, false);
					driver.Initialize(window.GetRawPtr(), EMsaaSamples::Samples_1, false);
					Require(!driver.IsInitialized() && driver.imageCalls == 0 && !driver.HasAnyDefault(),
						"unsupported capabilities must stop initialization before fallback resources");
					Require(!VulkanApi::GetInstance(), "failed capability initialization must release the Vulkan instance");
					Require(overrides.deviceCreateCalls == creates + deviceCreates && overrides.deviceDestroyCalls == destroys + deviceDestroys,
						"failed initialization must destroy exactly the logical devices it successfully created");
					Require(overrides.instanceCreateCalls == instanceCreates + instances && overrides.instanceDestroyCalls == instanceDestroys + instances &&
						overrides.surfaceDestroyCalls == surfaces + instances, "failed initialization must release its native instance and surface");
					Require(overrides.samplerCreateCalls == samplers && overrides.bufferCreateCalls == buffers,
						"capability refusal must precede self-retaining sampler caches and buffer creation");
				};
				for (uint32_t version : { VK_API_VERSION_1_2, VK_API_VERSION_1_3 })
				{
					overrides.deviceApiVersion = version;
					for (uint32_t missing : { 1u, 2u, 3u })
					{
						overrides.hiddenRenderingCommands = 0;
						overrides.missingRenderingCommands = missing;
						expectFailure(1u, 1u);
						overrides.missingRenderingCommands = 0;
						Require(overrides.hiddenRenderingCommands > 0, "capability fixture must intercept the actual device dispatch lookup");
					}
					using Missing = Sailor::Tests::MissingVulkanFeature;
					for (auto missing : { Missing::Anisotropy, Missing::FirstInstance, Missing::IndependentBlend, Missing::RuntimeArray,
						Missing::SampledImageIndexing, Missing::VariableDescriptorCount, Missing::PartiallyBound, Missing::DynamicRendering })
					{
						const uint32_t queries = overrides.featureQueries;
						overrides.missingFeature = missing;
						expectFailure(0u, 0u);
						overrides.missingFeature = Missing::None;
						Require(overrides.featureQueries > queries, "capability fixture must intercept the actual feature query");
					}
				}
				for (uint32_t version : { VK_API_VERSION_1_0, VK_API_VERSION_1_1 })
				{
					overrides.deviceApiVersion = version;
					const uint32_t queries = overrides.featureQueries;
					expectFailure(0u, 0u);
					Require(overrides.featureQueries == queries, "old device APIs must be rejected before querying newer feature structures");
				}
				overrides.deviceApiVersion = 0;
				for (VkResult result : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
				{
					overrides.deviceCreationResult = result;
					expectFailure(1u, 0u);
				}
				overrides.deviceCreationResult = VK_SUCCESS;
				overrides.loaderApiVersion = VK_API_VERSION_1_0;
				expectFailure(0u, 0u, 0u);
				overrides.loaderApiVersion = 0;
				std::cout << "Native Vulkan capabilities: 27 refusals release native objects before caches and uploads\n";
			}
#else
			(void)capabilityTests;
#endif

			for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				for (uint32_t failureCall : { 1u, 2u })
				{
					BootstrapDriver driver(failureCall, lost ? VK_ERROR_DEVICE_LOST : error, waitFailure);
					driver.Initialize(window.GetRawPtr(), EMsaaSamples::Samples_1, false);
					Require(driver.imageCalls == failureCall && !driver.IsInitialized() && !driver.HasAnyDefault(),
						"failed bootstrap must stop at the failed upload without publishing either fallback");
					const bool pending = waitFailure && !lost;
					Require(driver.PendingCount() == (pending ? 1u : 0u) && driver.lastCommand.NumRefs() == (pending ? 2u : 1u),
						"bootstrap must retain accepted pending commands but release refused or lost work");
					if (waitFailure) Require(capturedFenceCompleted, "bootstrap failure fixture must finish its real GPU upload");
					if (pending)
					{
						driver.TrackResources_ThreadSafe();
						Require(driver.PendingCount() == 1u && driver.lastCommand.NumRefs() == 2u,
							"partial initialization must retain the upload across resource collection");
						{
							auto device = VulkanApi::GetInstance()->GetMainDevice();
							QueueWaitOverride refusal(device->GetGraphicsQueue(), error);
							Require(!driver.BeginConditionalDestroy() && driver.PendingCount() == 1u && driver.lastCommand.NumRefs() == 2u,
								"failed partial shutdown must retain the original pending upload");
						}
					}
					driver.ResetFailure();
					Require(driver.BeginConditionalDestroy() && driver.PendingCount() == 0u && driver.lastCommand.NumRefs() == 1u,
						"completed partial shutdown must release the tracked upload");
					driver.lastCommand.Clear();
				}
				if (lost) break;
			}
			Require(!VulkanApi::GetInstance(), "failed bootstrap destruction must release the Vulkan instance");
			{
				BootstrapDriver driver(0u, VK_SUCCESS, false);
				driver.Initialize(window.GetRawPtr(), EMsaaSamples::Samples_1, false);
				Require(driver.IsInitialized() && driver.imageCalls == 2u && driver.GetDefaultTexture() && driver.DefaultCubemap(),
					"fresh bootstrap must publish both fallback resources together");
				Require(ReadImage(driver, driver.GetDefaultTexture()->m_vulkan.m_image) == std::vector<uint32_t>{ 0x00e567ffu },
					"fallback texture color changed");
				for (uint32_t face = 0; face < 6u; ++face)
					Require(ReadImage(driver, driver.DefaultCubemap(), 0u, face) == std::vector<uint32_t>{ 0x00e567ffu },
						"fallback cubemap face color changed");
				driver.lastCommand.Clear();
				Require(driver.BeginConditionalDestroy(), "successful bootstrap did not drain");
			}
			Require(App::Shutdown(), "bootstrap harness did not shut down");
			App::Initialize(argv, argc);
			Require(App::IsRendererInitialized(), "normal App must initialize after failed backend bootstraps");
			std::cout << "Native fallback bootstrap test passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		if (!App::Shutdown()) result = 1;
		return result;
	}

#if defined(__APPLE__)
	int RunAppBootstrapGpu(int argc, const char** argv)
	{
		auto& overrides = Tests::GetVulkanCapabilityOverrides();
		uint32_t bootstraps = 0;
		const auto resetFailure = [&]()
			{
				overrides.queueSubmit = nullptr;
				overrides.waitForFences = nullptr;
				overrides.getFenceStatus = nullptr;
				rejectNativeSubmit = false;
				captureNextFenceWait = false;
				observedFences = {};
			};
		const auto checkReleased = [&]()
			{
				++bootstraps;
				Require(!App::GetInstance() && !VulkanApi::GetInstance(), "App teardown must release its renderer and Vulkan instance");
				Require(overrides.deviceCreateCalls == bootstraps && overrides.deviceDestroyCalls == bootstraps &&
					overrides.instanceCreateCalls == bootstraps && overrides.instanceDestroyCalls == bootstraps &&
					overrides.surfaceDestroyCalls == bootstraps,
					"App teardown must destroy every native device, surface and instance");
				Require(overrides.bufferCreateCalls > 0u && overrides.imageCreateCalls > 0u && overrides.fenceCreateCalls > 0u,
					"native lifetime counters must observe the constructed resources");
				Require(overrides.bufferCreateCalls == overrides.bufferDestroyCalls &&
					overrides.imageCreateCalls == overrides.imageDestroyCalls &&
					overrides.fenceCreateCalls == overrides.fenceDestroyCalls,
					"App teardown must release all native buffers, images and fences after failed construction");
			};
		int result = 1;
		try
		{
			std::vector<const char*> arguments(argv, argv + argc);
			arguments.insert(arguments.end(), { "--editor", "--port", "0", "--new-world" });
			forwardNativeSubmit = reinterpret_cast<PFN_vkQueueSubmit>(dlsym(RTLD_DEFAULT, "vkQueueSubmit"));
			forwardFenceStatus = reinterpret_cast<PFN_vkGetFenceStatus>(dlsym(RTLD_DEFAULT, "vkGetFenceStatus"));
			forwardFenceWait = reinterpret_cast<PFN_vkWaitForFences>(dlsym(RTLD_DEFAULT, "vkWaitForFences"));
			Require(forwardNativeSubmit && forwardFenceStatus && forwardFenceWait,
				"the native bootstrap fixture must resolve the real driver dispatch");
			for (bool waitFailure : { false, true })
			{
				for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST })
				{
					for (uint32_t upload : { 1u, 2u })
					{
						overrides.queueSubmit = NativeSubmit;
						overrides.waitForFences = NativeFenceWait;
						overrides.getFenceStatus = NativeFenceStatus;
						overrides.lastUploadLayers = 0;
						submitCalls = fenceWaitCalls = allFenceWaitCalls = 0u;
						submitsBeforeFailure = waitsBeforeCapture = upload - 1u;
						nextResult = fenceWaitResult = error;
						rejectNativeSubmit = !waitFailure;
						submitBeforeFailure = false;
						captureNextFenceWait = waitFailure;
						capturedFenceCompleted = false;
						observedFences = {};
						fenceResults = { VK_NOT_READY, VK_NOT_READY };

						const auto initialization = App::Initialize(arguments.data(), static_cast<int32_t>(arguments.size()));
						auto* renderer = App::GetSubmodule<Renderer>();
						Require(initialization == EAppInitializationResult::Failed && App::GetExitCode() != 0 &&
							renderer && !renderer->IsInitialized() && !App::IsRendererInitialized(),
							"ordinary App construction must report the actual Renderer upload failure");
						auto* driver = dynamic_cast<VulkanGraphicsDriver*>(Renderer::GetDriver().GetRawPtr());
						Require(driver && !driver->IsInitialized() && !driver->GetDefaultTexture() &&
							!App::GetSubmodule<AssetRegistry>() && !App::GetSubmodule<EngineLoop>(),
							"failed Renderer construction must not publish a fallback or initialize later subsystems");
						Require(overrides.lastUploadLayers == (upload == 1u ? 1u : 6u),
							"the failure must reach the actual 2D or six-face fallback upload");
						Require(waitFailure ? capturedFenceCompleted && fenceWaitCalls == 1u :
							submitCalls == 1u && lastCommandCount == 1u && allFenceWaitCalls == upload - 1u,
							"the fixture must observe native refusal or finish accepted work before injecting a wait failure");
						rejectNativeSubmit = true;
						nextResult = VK_SUCCESS;
						submitsBeforeFailure = 0u;
						const auto submissions = submitCalls;
						App::Start();
						Require(submitCalls == submissions && App::Initialize() == EAppInitializationResult::Failed,
							"a failed App must neither enter its frame loop nor become Ready on another Initialize call");

						if (waitFailure && error != VK_ERROR_DEVICE_LOST)
						{
							auto* app = App::GetInstance();
							auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
							auto device = VulkanApi::GetInstance()->GetMainDevice();
							Require(forwardFenceStatus(*device, observedFences[0]) == VK_SUCCESS,
								"the pending-state fixture must have completed the physical GPU work");
							const uint32_t images = overrides.imageDestroyCalls, fences = overrides.fenceDestroyCalls;
							driver->TrackResources_ThreadSafe();
							Require(overrides.imageDestroyCalls == images && overrides.fenceDestroyCalls == fences,
								"ordinary collection must retain the pending fallback upload");
							{
								QueueWaitOverride refusal(device->GetGraphicsQueue(), error);
								Require(!App::Shutdown() && App::GetInstance() == app && App::GetSubmodule<Renderer>() == renderer &&
									App::GetSubmodule<Tasks::Scheduler>() == scheduler && overrides.imageDestroyCalls == images &&
									overrides.fenceDestroyCalls == fences,
									"a refused partial App shutdown must preserve the original renderer, scheduler and pending resources");
							}
							fenceResults[0] = VK_SUCCESS;
						}
						resetFailure();
						Require(App::Shutdown(), "App must finish partial teardown after the failure is removed");
						checkReleased();

						Require(App::Initialize(arguments.data(), static_cast<int32_t>(arguments.size())) == EAppInitializationResult::Ready &&
							App::IsRendererInitialized(), "a fresh App must initialize after each failed renderer construction");
						{
							auto& retry = *Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
							auto fallback = retry.GetDefaultTexture();
							Require(fallback && fallback->IsReady() && ReadImage(retry, fallback->m_vulkan.m_image) ==
								std::vector<uint32_t>{ 0x00e567ffu }, "fresh App fallback pixels must retain their original value");
							auto command = retry.CreateCommandList(false, ECommandListQueue::Graphics);
							retry.BeginCommandList(command, true);
							auto target = retry.CreateRenderTarget(command, glm::ivec2(4), 1, ETextureFormat::R8G8B8A8_UNORM);
							retry.BeginRenderPass(command, TVector<RHITexturePtr>{ target }, nullptr, glm::ivec4(0, 0, 4, 4),
								glm::ivec2(0), true, glm::vec4(0x43 / 255.0f, 0x67 / 255.0f, 0xab / 255.0f, 1.0f), 0.0f, false);
							retry.EndRenderPass(command);
							retry.RestoreImageBarriers(command);
							retry.EndCommandList(command);
							Require(retry.SubmitCommandList_Immediate(command), "fresh App must execute an offscreen rendering pass");
							Require(ReadImage(retry, target->m_vulkan.m_image) == std::vector<uint32_t>(16, 0xffab6743u),
								"fresh App must preserve every rendered retry pixel");
						}
						Require(App::Shutdown(), "the successful retry must shut down through App");
						checkReleased();
						std::cout << "App bootstrap failure: upload=" << upload << ", wait=" << waitFailure << ", result=" << error
							<< "; owner teardown, fresh initialization and 16 rendered pixels passed\n";
					}
				}
			}
			std::cout << "Native App bootstrap resource lifetimes: " << bootstraps << " devices, " << overrides.bufferCreateCalls
				<< " buffers, " << overrides.imageCreateCalls << " images, " << overrides.fenceCreateCalls << " fences\n";
			std::cout << "Native App bootstrap: 12 upload failures, four retained shutdown retries and complete resource release passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		resetFailure();
		App::Stop();
		if (!App::Shutdown()) result = 1;
		return result;
	}
#endif

	void TestImmediateBufferCopy(bool lost)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		std::array<uint32_t, 64> data{};
		for (uint32_t i = 0u; i < data.size(); ++i) data[i] = 719u + 97u * i;
		auto createSource = [&]()
			{
				return driver->CreateBuffer_Immediate(data.data(), sizeof(data), EBufferUsageBit::BufferTransferSrc_Bit);
			};
		auto createDestination = [&]()
			{
				auto result = driver->CreateBuffer(sizeof(data), EBufferUsageBit::BufferTransferDst_Bit,
					EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
				std::fill_n(static_cast<uint32_t*>(result->GetPointer()), data.size(), 0xdeadbeefu);
				return result;
			};
		auto source = createSource();
		auto destination = createDestination();
		Require(source.IsValid(), "successful immediate creation must publish a buffer");
		Require(driver->CopyBuffer_Immediate(source, destination, 31u * sizeof(uint32_t), 7u * sizeof(uint32_t), 11u * sizeof(uint32_t)),
			"immediate subrange copy did not complete");
		const auto* copied = static_cast<const uint32_t*>(destination->GetPointer());
		for (uint32_t i = 0u; i < data.size(); ++i)
			Require(copied[i] == (i >= 11u && i < 42u ? data[i - 11u + 7u] : 0xdeadbeefu),
				"immediate copy must preserve offsets and untouched neighboring bytes");
		Require(driver->CopyBuffer_Immediate(source, destination, sizeof(data)), "immediate full copy did not complete");
		Require(std::equal(data.begin(), data.end(), copied), "immediate create/copy data mismatch");
		source.Clear();
		destination.Clear();

		using BufferOwner = Memory::TManagedMemory<Memory::VulkanBufferMemoryPtr, RHIBuffer::VulkanBufferAllocator>;
		for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			if (!lost)
			{
				auto refusedDestination = createDestination();
				auto refusedSource = createSource();
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device, true), error);
					Require(!createSource(), "rejected immediate creation must not publish a buffer");
					Require(!driver->CopyBuffer_Immediate(refusedSource, refusedDestination, sizeof(data)),
						"immediate copy must propagate submission refusal");
				}
				const auto* untouched = static_cast<const uint32_t*>(refusedDestination->GetPointer());
				Require(std::all_of(untouched, untouched + data.size(), [](uint32_t value) { return value == 0xdeadbeefu; }),
					"rejected copy changed its destination");
				Require(driver->CopyBuffer_Immediate(refusedSource, refusedDestination, sizeof(data)), "copy retry did not complete");
				Require(std::equal(data.begin(), data.end(), untouched), "copy retry data mismatch");
			}
			auto pendingSource = createSource();
			auto pendingDestination = createDestination();
			TWeakPtr<BufferOwner> sourceOwner(pendingSource->m_vulkan.m_buffer);
			TWeakPtr<BufferOwner> destinationOwner(pendingDestination->m_vulkan.m_buffer);
			{
				FenceDispatchOverride dispatch(*device);
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : error;
				captureNextFenceWait = true;
				capturedFenceCompleted = false;
				Require(!driver->CopyBuffer_Immediate(pendingSource, pendingDestination, sizeof(data)) && capturedFenceCompleted,
					"immediate copy must report an accepted wait failure");
				Require(std::equal(data.begin(), data.end(), static_cast<const uint32_t*>(pendingDestination->GetPointer())),
					"the fault fixture must finish its real copy before simulating loss");
				pendingSource.Clear();
				pendingDestination.Clear();
				driver->TrackResources_ThreadSafe();
				Require(static_cast<bool>(sourceOwner.TryLock()) == !lost && static_cast<bool>(destinationOwner.TryLock()) == !lost,
					"pending copies must retain original managed allocations; terminal loss must release them");
				if (!lost)
				{
					fenceResults[0] = VK_SUCCESS;
					driver->TrackResources_ThreadSafe();
					Require(!sourceOwner.TryLock() && !destinationOwner.TryLock(), "completed copies must release both allocation owners");
				}
			}
			if (lost) break;
		}
	}

	struct ParticleBufferNode : Framegraph::Experimental::ParticlesNode
	{
		using ParticlesNode::InitializeBuffers;
		using ParticlesNode::m_instances;
		using ParticlesNode::m_particlesFrames;
		using ParticlesNode::m_particlesDataBinary;
		using ParticlesNode::m_perInstanceData;
		using ParticlesNode::m_numInstances;
		using ParticlesNode::m_particlesHeader;
		using ParticlesNode::m_shadowMap;
		using ParticlesNode::m_shadowMapBinding;
	};

	void TestParticleShadowInitialization()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			ParticleBufferNode node;
			node.m_particlesHeader.m_bIsLoaded = true;
			node.m_particlesDataBinary.Resize(1);
			node.m_particlesDataBinary[0] = {};
			node.m_particlesDataBinary[0].m_x2 = 3.5f;
			auto graph = RHIFrameGraphPtr::Make();
			auto draw = driver.CreateCommandList(false, ECommandListQueue::Graphics);
			driver.BeginCommandList(draw, true);
			{
				SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
				const auto before = nativeSubmitAttempts;
				node.Process(graph, draw, draw, {});
				Require(nativeSubmitAttempts == before + 1 && !node.m_shadowMap && !node.m_shadowMapBinding &&
					draw->GetNumRecordedCommands() == 0 && node.GetDrawCallStats().m_numBatches == 0 &&
					node.m_particlesHeader.m_bIsLoaded && node.m_particlesDataBinary.Num() == 1 &&
					node.m_particlesDataBinary[0].m_x2 == 3.5f,
					"a refused particle shadow target must retain the loaded source without publishing or drawing");
			}
			node.Process(graph, draw, draw, {});
			const auto target = node.m_shadowMap;
			auto bindings = node.m_shadowMapBinding;
			Require(target && bindings && target->GetExtent() == glm::ivec2(4096) &&
				bindings->GetOrAddShaderBinding("shadowMapSampler"_h)->GetTextureBinding() == target,
				"the same particle node must retry and publish its real shadow image");
			{
				SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
				const auto before = nativeSubmitAttempts;
				node.Process(graph, draw, draw, {});
				Require(nativeSubmitAttempts == before && node.m_shadowMap == target && node.m_shadowMapBinding == bindings,
					"warm particle setup must retain the accepted image and descriptor identities");
			}
			driver.ImageMemoryBarrier(draw, target, EImageLayout::TransferDstOptimal);
			driver.ClearImage(draw, target, glm::vec4(0.375f));
			driver.RestoreImageBarriers(draw);
			driver.EndCommandList(draw);
			Require(driver.SubmitCommandList_Immediate(draw), "the recovered particle image must accept GPU writes");
			const auto pixels = ReadImage(driver, target->m_vulkan.m_image);
			Require(std::all_of(pixels.begin(), pixels.end(), [](uint32_t pixel) { return pixel == 0x3ec00000u; }),
				"all recovered particle shadow pixels must contain the submitted value");
		}
		std::cout << "Particle shadow target: native refusal, retained source, same-node retry, warm bindings and complete pixels passed\n";
	}

	void TestParticleBufferPublication()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		TVector<ParticleBufferNode::PerInstanceData> instances(3u);
		for (auto& instance : instances)
		{
			instance = {};
			instance.model = glm::mat4(1.0f);
			instance.color = glm::vec4(1.0f);
		}
		for (uint32_t failedUpload : { 0u, 1u })
		{
			for (bool waitFailure : { false, true })
			{
				for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
				{
					ParticleBufferNode node;
					node.m_particlesDataBinary.Resize(3u);
					for (auto& particle : node.m_particlesDataBinary) particle = {};
					node.m_particlesDataBinary[1].m_x2 = 123.5f;
					if (waitFailure)
					{
						FenceDispatchOverride dispatch(*device);
						fenceResults = { VK_NOT_READY, VK_NOT_READY };
						fenceWaitResult = error;
						waitsBeforeCapture = failedUpload;
						captureNextFenceWait = true;
						capturedFenceCompleted = false;
						Require(!node.InitializeBuffers(instances) && capturedFenceCompleted,
							"particle initialization must propagate either upload's wait failure");
						fenceResults[0] = VK_SUCCESS;
						driver->TrackResources_ThreadSafe();
					}
					else
					{
						SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device, true), error, false, failedUpload);
						Require(!node.InitializeBuffers(instances), "particle initialization must propagate either upload's submission refusal");
					}
					Require(!node.m_instances && !node.m_particlesFrames && !node.m_perInstanceData && node.m_numInstances == 0u &&
						node.m_particlesDataBinary.Num() == 3u && node.m_particlesDataBinary[1].m_x2 == 123.5f,
						"partial particle initialization must publish nothing and retain its CPU source for retry");
					Require(node.InitializeBuffers(instances), "particle initialization retry did not complete");
					Require(node.m_instances && node.m_particlesFrames && node.m_perInstanceData && node.m_numInstances == 3u &&
						node.m_particlesDataBinary.IsEmpty(), "successful particle initialization must publish both buffers and consume its CPU source");
					Require(node.m_perInstanceData->GetOrAddShaderBinding("data"_h)->m_vulkan.m_valueBinding == node.m_instances->m_vulkan.m_buffer &&
						node.m_perInstanceData->GetOrAddShaderBinding("particlesData"_h)->m_vulkan.m_valueBinding == node.m_particlesFrames->m_vulkan.m_buffer,
						"particle bindings must retain the published buffers' original allocations");
				}
			}
		}
	}

	int RunInitializationGpu(int argc, const char** argv)
	{
		Tests::TempDirectory fixture("host-initialization");
		const auto workspaceRoot = fixture.Path(Workspace::PathFromUtf8(
			reinterpret_cast<const char*>(u8"Workspace \u042f \u00e9 \u8239 \U0001f6a2")));
		int result = 1;
		try
		{
			std::filesystem::create_directories(workspaceRoot / "Content");
			std::string enginePath = Workspace::PathToUtf8(std::filesystem::current_path());
			for (int i = 1; i + 1 < argc; ++i)
				if (std::string_view(argv[i]) == "--workspace") enginePath = argv[i + 1];
			YAML::Node manifest;
			manifest["manifestVersion"] = 1;
			manifest["workspaceId"] = "00000000-0000-0000-0000-000000000120";
			manifest["name"] = "Initialization test";
			manifest["enginePath"] = enginePath;
			manifest["engineReferenceKind"] = "source";
			manifest["contentPath"] = "Content";
			manifest["sourcePath"] = "Source";
			manifest["generatedProjectPath"] = "Generated";
			manifest["cachePath"] = "Cache";
			manifest["buildPath"] = "Cache/Build";
			manifest["logicOutputPath"] = "Binaries";
			manifest["logicModuleName"] = "MissingInitializationModule";
			std::ofstream(workspaceRoot / "workspace.sailor") << manifest;
			const std::string modelName = reinterpret_cast<const char*>(u8"Duck \u042f \u00e9 \u8239 \U0001f6a2.glb");
			const auto modelPath = workspaceRoot / "Content" / Workspace::PathFromUtf8(modelName);
			std::filesystem::copy_file(Workspace::PathFromUtf8(enginePath) / "Content/Models/DuckGlb/Duck.glb",
				modelPath);
			YAML::Node model;
			model["assetInfoType"] = "Sailor::ModelAssetInfo";
			model["fileId"] = "{00000000-0000-0000-0000-000000000120}";
			model["filename"] = modelName;
			model["bShouldGenerateMaterials"] = true;
			model["bShouldKeepCpuBuffers"] = true;
			model["bGenerateBLAS"] = true;
			model["unitScale"] = 1;
			auto modelMetadataPath = modelPath;
			modelMetadataPath += ".asset";
			std::ofstream(modelMetadataPath) << model;
			const std::string workspacePath = Workspace::PathToUtf8(workspaceRoot);
			const auto outputPath = workspaceRoot / Workspace::PathFromUtf8(
				reinterpret_cast<const char*>(u8"Image \u042f \u00e9 \u8239 \U0001f6a2.png"));
			const std::string output = Workspace::PathToUtf8(outputPath);
			std::vector<const char*> commandArguments(argv, argv + argc);
			commandArguments.insert(commandArguments.end(), { "--workspace", workspacePath.c_str(), "--editor", "--port", "0",
				"--pathtracer", "--in", modelName.c_str(),
				"--out", output.c_str(), "--height", "8", "--samples", "1", "--ambientSamples", "1", "--bounces", "1" });
			Require(App::Initialize(commandArguments.data(), static_cast<int32_t>(commandArguments.size())) ==
				EAppInitializationResult::Completed && std::filesystem::is_regular_file(outputPath) &&
				std::filesystem::file_size(outputPath) > 32u, "an offline command must complete without claiming an interactive session");
			TextureImporter::CpuDecodeRequest image;
			image.m_filepath = output;
			FileRevision imageRevision;
			Require(Utils::TryGetFileRevision(output, imageRevision), "the PNG must have a capturable source revision");
			image.m_sourceRevisions.Add(output, imageRevision);
			TextureImporter::ByteCode pixels;
			int32_t width = 0, height = 0;
			uint32_t mips = 0;
			Require(TextureImporter::DecodeTextureCpu(image, pixels, width, height, mips) && height == 8 &&
				width > 0 && pixels.Num() == static_cast<size_t>(width * height * 4),
				"the texture importer must decode the actual PNG written through a Unicode filename");
			const auto canonicalRoot = std::filesystem::canonical(workspaceRoot);
			Require(App::GetWorkspaceContext().GetRoot() == canonicalRoot &&
				App::GetWorkspace() == Workspace::PathToUtf8(canonicalRoot) + "/",
				"standalone command bootstrap must retain the actual Unicode workspace root");
			App::Start();
			Require(!App::GetSubmodule<EngineLoop>(), "a completed command must not create or enter a game loop");
			Require(App::Shutdown() && !App::GetInstance(), "a completed command must release its App before another startup");

			const char* missingWorld = "Tests/Visual/MissingRequiredStartup.world";
			std::vector<const char*> arguments(argv, argv + argc);
			arguments.insert(arguments.end(), { "--world", missingWorld });
			const auto initialization = App::Initialize(arguments.data(), static_cast<int32_t>(arguments.size()));
			Require(initialization == EAppInitializationResult::Failed && App::IsRendererInitialized() && App::GetExitCode() != 0,
				"a missing required startup world must fail even after the renderer has initialized");
			App::Start();
			Require(App::GetSubmodule<EngineLoop>()->GetWorlds().IsEmpty() &&
				App::Initialize() == EAppInitializationResult::Failed, "failed startup must not create a fallback game world or become Ready");
			Require(App::Shutdown() && !App::GetInstance(), "failed startup must release its GPU and App resources");

			std::string initialize;
			for (int i = 0; i < argc; ++i) Tests::ProtocolWire::AppendBytesField(initialize, 1u, argv[i]);
			for (const auto& argument : { std::string("--workspace"), workspacePath,
				std::string("--new-world"), std::string("--port"), std::string("0") })
			{
				Tests::ProtocolWire::AppendBytesField(initialize, 1u, argument);
			}
			Require(ix::initNetSystem(), "port reservation must initialize the network system");
			const int port = ix::getFreePort();
			Require(ix::uninitNetSystem() && port > 0 && port <= 65535, "port reservation must complete");
			const std::string token = "0123456789abcdef0123456789abcdef";
			auto startHost = [&]()
				{
					const std::string payload = Tests::ProtocolWire::MakeRequest(1u, 10u, initialize);
					return static_cast<Protocol::EEditorEngineWebSocketHostStatus>(SailorProtocolStartLocalHost(
						reinterpret_cast<const uint8_t*>(payload.data()), static_cast<uint32_t>(payload.size()),
						static_cast<uint16_t>(port), token.data(), static_cast<uint32_t>(token.size())));
				};
			Require(startHost() == Protocol::EEditorEngineWebSocketHostStatus::InitializationFailed && !App::GetInstance(),
				"a game session without its required module must fail and roll back its native host");

			Tests::ProtocolWire::AppendBytesField(initialize, 1u, "--editor");
			for (uint32_t attempt = 0u; attempt < 2u; ++attempt)
			{
				Require(startHost() == Protocol::EEditorEngineWebSocketHostStatus::Ok,
					"the editor must start without its workspace module, including after shutdown and retry");
				auto* loop = App::GetSubmodule<EngineLoop>();
				Require(App::IsRendererInitialized() && App::HasEditor() && loop && loop->GetWorlds().Num() == 1u &&
					App::GetLoadedWorldPath().empty(), "limited editor mode must still own a renderer and editable empty world");
				Require(App::GetWorkspaceContext().GetRoot() == canonicalRoot &&
					App::GetWorkspace() == Workspace::PathToUtf8(canonicalRoot) + "/",
					"editor protocol bootstrap must preserve the same Unicode workspace as standalone arguments");
				Require(App::Initialize() == EAppInitializationResult::Ready,
					"an already initialized editor must retain its successful result");
				App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker,
					EThreadType::RHI, EThreadType::Render, EThreadType::Editor });

				SailorProtocolRequestLocalHostStop();
				const std::string payload = Tests::ProtocolWire::MakeRequest(2u, 11u);
				uint8_t* responseData = nullptr;
				uint32_t responseSize = 0u;
				const auto status = SailorProtocolInvoke(reinterpret_cast<const uint8_t*>(payload.data()),
					static_cast<uint32_t>(payload.size()), &responseData, &responseSize);
				std::string responsePayload;
				if (responseData) responsePayload.assign(reinterpret_cast<const char*>(responseData), responseSize);
				SailorProtocolFreeBuffer(responseData);
				Tests::ProtocolWire::TProtocolResponseWire response;
				Require(status == static_cast<int32_t>(Protocol::EEditorEngineTransportStatus::Ok) &&
					Tests::ProtocolWire::ParseResponse(responsePayload, response) &&
					response.m_protocolVersion == Protocol::EditorEngineProtocolVersion && response.m_requestId == 2u &&
					!response.m_bSuccess && !response.m_error.empty(),
					"the real native Stop request must prevent a later protocol Start");
				Require(SailorProtocolStopLocalHost(true) != 0 && !App::GetInstance(),
					"editor host shutdown must release its initialized App before retry");
			}
			std::cout << "Native initialization outcomes: Unicode workspace, offline output and repeated editor protocol startup passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		if (SailorProtocolStopLocalHost(true) == 0) result = 1;
		return result;
	}

	int RunShutdownGpu(int argc, const char** argv, bool idleFailure, bool localHost = false)
	{
		App::Initialize(argv, argc);
		int result = 1;
		auto shutdown = [localHost]() { return localHost ? SailorProtocolStopLocalHost(true) != 0 : App::Shutdown(); };
		try
		{
			Require(App::IsRendererInitialized(), "shutdown test requires an initialized renderer");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			{
				FailedFrame failed;
				OnRender([&]() { TestNativeFrameFailure(true, VK_ERROR_OUT_OF_HOST_MEMORY, failed); });
				auto device = VulkanApi::GetInstance()->GetMainDevice();
				auto swapchain = device->GetSwapchain();
				auto* renderer = App::GetSubmodule<Renderer>();
				auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
				for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
				{
					bool stopped;
					if (idleFailure)
					{
						const auto before = queueWaitCalls;
						QueueWaitOverride refusal(device->GetGraphicsQueue(), error);
						stopped = shutdown();
						Require(queueWaitCalls > before, "shutdown must reach the actual queue-idle failure");
					}
					else
					{
						const auto before = submitCalls;
						SubmitOverride refusal(device->GetGraphicsQueue(), error);
						stopped = shutdown();
						Require(submitCalls == before + 1u && lastWait == *failed.acquire &&
							lastSignal == VK_NULL_HANDLE && lastCommandCount == 0u,
							"shutdown must consume the outstanding acquisition before releasing its owners");
					}
					Require(!stopped && App::GetSubmodule<Renderer>() == renderer && App::GetSubmodule<Tasks::Scheduler>() == scheduler,
						"failed shutdown must retain the App, renderer and scheduler for retry");
					Require(device->GetSwapchain() == swapchain, "failed shutdown must retain the swapchain");
					VulkanSubmissionTestAccess::CheckSyncCounts(*device);
					Require(VulkanSubmissionTestAccess::FlightFence(*device) == failed.fence &&
						failed.frame.command->m_vulkan.m_commandBuffer.NumRefs() == 2u,
						"failed shutdown must retain frame synchronization and command dependencies");
					CheckReadback(failed.frame, false);
				}
			}
			// Release test-owned native references before the successful App teardown.
			Require(shutdown() && App::GetSubmodule<Renderer>() == nullptr && App::GetSubmodule<Tasks::Scheduler>() == nullptr,
				"shutdown must complete after the refusal is removed");
			Require(shutdown(), "a completed shutdown must remain idempotent");
			App::Initialize(argv, argc);
			Require(App::IsRendererInitialized(), "a completed shutdown must allow a new native session");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			OnRender([]() { TestImmediateSubmission(false); });
			std::cout << "Native shutdown ownership test passed: " << (idleFailure ? "idle" : "acquire") << (localHost ? " host" : " App") << '\n';
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		if (!shutdown()) result = 1;
		return result;
	}

#if defined(__APPLE__)
	struct ImGuiAllocationProbe
	{
		ImGuiMemAllocFunc m_allocate{};
		ImGuiMemFreeFunc m_free{};
		void* m_userData{};
		std::atomic<int64_t> m_liveAllocations{ 0 };
		std::atomic<uint64_t> m_totalAllocations{ 0 };

		ImGuiAllocationProbe()
		{
			ImGuiApi::GetAllocatorFunctions(&m_allocate, &m_free, &m_userData);
			ImGui::SetAllocatorFunctions(Allocate, Free, this);
		}

		~ImGuiAllocationProbe()
		{
			ImGui::SetAllocatorFunctions(m_allocate, m_free, m_userData);
		}

		static void* Allocate(size_t size, void* userData)
		{
			auto& probe = *static_cast<ImGuiAllocationProbe*>(userData);
			void* allocation = probe.m_allocate(size, probe.m_userData);
			if (allocation)
			{
				++probe.m_liveAllocations;
				++probe.m_totalAllocations;
			}
			return allocation;
		}

		static void Free(void* allocation, void* userData)
		{
			if (!allocation) return;
			auto& probe = *static_cast<ImGuiAllocationProbe*>(userData);
			probe.m_free(allocation, probe.m_userData);
			--probe.m_liveAllocations;
		}
	};

	int RunImGuiLifetimeGpu(int argc, const char** argv)
	{
		ImGuiAllocationProbe allocations;
		struct ShutdownProbe
		{
			TWeakPtr<const ImGuiApi::PreparedFrame> m_frame;
			std::atomic<bool> m_bReaderFinished{ false };
			bool m_bWasDestroyed = false;
			bool m_bReadersDrained = false;
			std::thread::id m_owner = std::this_thread::get_id();
		} shutdown;
		int result = 1;
		try
		{
			ImGuiMemAllocFunc allocate{};
			ImGuiMemFreeFunc free{};
			void* userData{};
			ImGuiApi::GetAllocatorFunctions(&allocate, &free, &userData);
			Require(allocate == ImGuiAllocationProbe::Allocate && free == ImGuiAllocationProbe::Free && userData == &allocations,
				"the allocation probe must observe the engine's ImGui, not an executable-local copy");
			Require(!ImGuiApi::GetCurrentContext(), "the process must start without an engine ImGui context");

			std::string initialize;
			for (int i = 0; i < argc; ++i) Tests::ProtocolWire::AppendBytesField(initialize, 1u, argv[i]);
			for (const auto argument : { "--editor", "--port", "0", "--world", "", "--new-world" })
				Tests::ProtocolWire::AppendBytesField(initialize, 1u, argument);
			Require(ix::initNetSystem(), "ImGui lifetime fixture must initialize local networking");
			const int port = ix::getFreePort();
			Require(ix::uninitNetSystem() && port > 0 && port <= 65535, "ImGui lifetime fixture must reserve a local port");
			const auto start = [&](const std::string& payload)
			{
				const auto request = Tests::ProtocolWire::MakeRequest(1u, 10u, payload);
				constexpr std::string_view token = "0123456789abcdef0123456789abcdef";
				return static_cast<Protocol::EEditorEngineWebSocketHostStatus>(SailorProtocolStartLocalHost(
					reinterpret_cast<const uint8_t*>(request.data()), static_cast<uint32_t>(request.size()),
					static_cast<uint16_t>(port), token.data(), static_cast<uint32_t>(token.size())));
			};

			std::string invalidInitialize = initialize;
			// An incomplete offline command fails after renderer setup, before ImGui creation.
			Tests::ProtocolWire::AppendBytesField(invalidInitialize, 1u, "--pathtracer");
			Require(start(invalidInitialize) == Protocol::EEditorEngineWebSocketHostStatus::InitializationFailed &&
				!App::GetInstance() && !ImGuiApi::GetCurrentContext() && allocations.m_liveAllocations == 0,
				"failed bootstrap must roll back without creating or retaining an ImGui context");

			for (uint32_t cycle = 0; cycle < 24; ++cycle)
			{
				Require(start(initialize) == Protocol::EEditorEngineWebSocketHostStatus::Ok,
					"each native host restart must initialize successfully");
				auto* context = ImGuiApi::GetCurrentContext();
				Require(context && allocations.m_liveAllocations > 0, "the native host must allocate a real ImGui context");
				ImGui::SetCurrentContext(context);
				ImGui::GetIO().IniFilename = nullptr;

				shutdown.m_bReaderFinished = false;
				shutdown.m_bWasDestroyed = false;
				shutdown.m_bReadersDrained = false;
				ImGuiContextHook hook{};
				hook.Type = ImGuiContextHookType_Shutdown;
				hook.UserData = &shutdown;
				hook.Callback = [](ImGuiContext*, ImGuiContextHook* activeHook)
				{
					auto& probe = *static_cast<ShutdownProbe*>(activeHook->UserData);
					probe.m_bWasDestroyed = true;
					probe.m_bReadersDrained = probe.m_bReaderFinished && !probe.m_frame.TryLock() &&
						probe.m_owner == std::this_thread::get_id();
				};
				ImGui::AddContextHook(context, &hook);

				auto* imGui = App::GetSubmodule<ImGuiApi>();
				imGui->NewFrame();
				ImGui::GetForegroundDrawList()->AddText({ 10, 10 }, IM_COL32_WHITE, "Retained shutdown frame");
				ImGuiApi::PreparedFramePtr frame;
				{
					auto command = Renderer::GetDriver()->CreateCommandList(false, ECommandListQueue::Transfer);
					auto commands = Renderer::GetDriverCommands();
					commands->BeginCommandList(command, true);
					frame = imGui->PrepareFrame(command);
					commands->EndCommandList(command);
					Require(frame && frame->DrawData.GetDrawData().TotalVtxCount > 0 &&
						Renderer::GetDriver()->SubmitCommandList_Immediate(command),
						"the retained frame must contain real uploaded ImGui geometry");
				}
				shutdown.m_frame = frame;
				std::atomic<bool> bReaderEntered{ false }, bReleaseReader{ false };
				Tasks::CreateTask("Retain ImGui frame during shutdown"_h,
					[frame = std::move(frame), &shutdown, &bReaderEntered, &bReleaseReader]()
					{
						bReaderEntered = true;
						bReaderEntered.notify_one();
						bReleaseReader.wait(false);
						auto commands = Renderer::GetDriverCommands();
						auto draw = Renderer::GetDriver()->CreateCommandList(true, ECommandListQueue::Graphics);
						commands->BeginSecondaryCommandList(draw, false, false, App::GetSubmodule<Renderer>()->GetColorFormat());
						ImGuiApi::RenderFrame(frame, draw);
						commands->EndCommandList(draw);
						shutdown.m_bReaderFinished = frame->DrawData.GetDrawData().TotalVtxCount > 0;
					}, EThreadType::RHI)->Run();
				bReaderEntered.wait(false);
				Tasks::CreateTask("Release ImGui reader from shutdown drain"_h, [&]()
					{
						bReleaseReader = true;
						bReleaseReader.notify_one();
					}, EThreadType::Main)->Run();
				Require(SailorProtocolStopLocalHost(true) != 0, "the native host must join its pending ImGui reader");
				std::cout << "ImGui shutdown cycle " << cycle << ": live allocations=" << allocations.m_liveAllocations
					<< ", context=" << ImGuiApi::GetCurrentContext() << '\n';
				Require(shutdown.m_bWasDestroyed && shutdown.m_bReadersDrained && !ImGuiApi::GetCurrentContext() &&
					allocations.m_liveAllocations == 0 && !shutdown.m_frame.TryLock(),
					"shutdown must destroy its context on the CPU owner after RHI readers and release all ImGui allocations");
			}
			std::cout << "Native ImGui lifetime: 24 host cycles, pending RHI readers and failed bootstrap passed; allocations="
				<< allocations.m_totalAllocations << '\n';
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		if (SailorProtocolStopLocalHost(true) == 0) result = 1;
		return result;
	}

	int RunImGuiWorkspaceGpu(int argc, const char** argv)
	{
		Tests::TempDirectory workspace("imgui-workspace");
		ImGuiAllocationProbe allocations;
		// Keep observer storage valid even if a failing loader retains an image until exit.
		static std::array<Tests::ImGuiWorkspaceProbe, 24> probes;
		int result = 1;
		try
		{
			std::filesystem::create_directories(workspace.Path("Content"));
			std::string enginePath = std::filesystem::current_path().string();
			for (int i = 1; i + 1 < argc; ++i)
				if (std::string_view(argv[i]) == "--workspace") enginePath = argv[i + 1];
			YAML::Node manifest;
			manifest["manifestVersion"] = 1;
			manifest["workspaceId"] = "00000000-0000-0000-0000-000000000261";
			manifest["name"] = "ImGui workspace lifetime";
			manifest["enginePath"] = enginePath;
			manifest["engineReferenceKind"] = "source";
			manifest["contentPath"] = "Content";
			manifest["sourcePath"] = "Source";
			manifest["generatedProjectPath"] = "Generated";
			manifest["cachePath"] = "Cache";
			manifest["buildPath"] = "Cache/Build";
			manifest["logicOutputPath"] = "Binaries";
			manifest["logicModuleName"] = "ImGuiWorkspaceFixture";
			std::ofstream(workspace.Path("workspace.sailor")) << manifest;
			const auto modulePath = workspace.Path("Binaries") / App::GetBuildConfig() / "libImGuiWorkspaceFixture.dylib";
			std::filesystem::create_directories(modulePath.parent_path());
			std::filesystem::copy_file(SAILOR_IMGUI_FIXTURE_PATH, modulePath);
			std::ofstream(workspace.Path("Content/EditorRenderer.renderer")) <<
				"renderTargets:\n"
				"- name: EditorOutput\n  format: B8G8R8A8_UNORM\n  width: 64\n  height: 48\n"
				"- name: UiDepth\n  format: D32_SFLOAT_S8_UINT\n  width: 64\n  height: 48\n"
				"frame:\n"
				"- name: Clear\n  vec4:\n  - clearColor: [0, 0, 0, 1]\n  renderTargets:\n  - target: EditorOutput\n"
				"- name: Clear\n  float:\n  - clearDepth: 0\n  - clearStencil: 0\n  renderTargets:\n  - target: UiDepth\n"
				"- name: RenderImGui\n  renderTargets:\n  - color: EditorOutput\n  - depthStencil: UiDepth\n";

			std::string initialize;
			for (int i = 0; i < argc; ++i) Tests::ProtocolWire::AppendBytesField(initialize, 1u, argv[i]);
			const std::string workspacePath = workspace.Get().string();
			for (const auto argument : { "--workspace", workspacePath.c_str(), "--editor", "--port", "0", "--new-world" })
				Tests::ProtocolWire::AppendBytesField(initialize, 1u, argument);
			const auto initializeRequest = Tests::ProtocolWire::MakeRequest(1u, 10u, initialize);
			Require(ix::initNetSystem(), "workspace fixture must initialize networking");
			const int port = ix::getFreePort();
			Require(ix::uninitNetSystem() && port > 0 && port <= 65535, "workspace fixture must reserve a local port");
			constexpr std::string_view token = "0123456789abcdef0123456789abcdef";
			for (auto& probe : probes)
			{
				Require(SailorProtocolStartLocalHost(reinterpret_cast<const uint8_t*>(initializeRequest.data()),
					static_cast<uint32_t>(initializeRequest.size()), static_cast<uint16_t>(port), token.data(),
					static_cast<uint32_t>(token.size())) == static_cast<int32_t>(Protocol::EEditorEngineWebSocketHostStatus::Ok),
					"the native host must load the actual workspace fixture through its manifest");
				Require(Reflection::TryGetTypeByName("ImGuiWorkspace::FixtureComponent"),
					"the App module manager must register the fixture's reflected component");
				auto* context = ImGuiApi::GetCurrentContext();
				ImGui::SetCurrentContext(context);
				ImGui::GetIO().IniFilename = nullptr;
				{
					Platform::DynamicLibrary module(modulePath);
					auto configure = reinterpret_cast<void (*)(Tests::ImGuiWorkspaceProbe*)>(module.GetSymbol("ConfigureImGuiWorkspaceProbe"));
					Require(configure, "the loaded fixture must expose its observer binding");
					configure(&probe);
					Require(probe.m_bHasPrivateContext, "the workspace must have a separate ImGui context binding before borrowing the engine's");
					Require(module.Close() && !probe.m_bWasModuleUnloaded, "only App may retain the module during its session");
				}
				App::SetEditorRenderTargetSize(64, 48);
				const auto start = Tests::ProtocolWire::MakeRequest(2u, 11u);
				uint8_t* responseData = nullptr;
				uint32_t responseSize = 0;
				const auto status = SailorProtocolInvoke(reinterpret_cast<const uint8_t*>(start.data()),
					static_cast<uint32_t>(start.size()), &responseData, &responseSize);
				std::string responseBytes;
				if (responseData) responseBytes.assign(reinterpret_cast<const char*>(responseData), responseSize);
				SailorProtocolFreeBuffer(responseData);
				Tests::ProtocolWire::TProtocolResponseWire response;
				Require(status == static_cast<int32_t>(Protocol::EEditorEngineTransportStatus::Ok) &&
					Tests::ProtocolWire::ParseResponse(responseBytes, response) && response.m_bSuccess,
					"the real protocol Start command must enter App::Start");
				probe.m_callbacksStarted.wait(0);
				Require(probe.m_frames > 0 && probe.m_callbacksFinished == 0 && !probe.m_bWasModuleUnloaded,
					"the running engine must reach an in-flight callback implemented by the loaded module");
				SailorProtocolRequestLocalHostStop();
				probe.m_bIsCallbackReleased = true;
				probe.m_bIsCallbackReleased.notify_all();
				Require(SailorProtocolStopLocalHost(false) != 0 && App::GetInstance(),
					"stopping the engine loop must retain its App until explicit shutdown");
				{
					auto device = VulkanApi::GetInstance()->GetMainDevice();
					QueueWaitOverride refusal(device->GetGraphicsQueue(), VK_ERROR_OUT_OF_HOST_MEMORY);
					const auto before = queueWaitCalls;
					Require(!App::Shutdown() && queueWaitCalls > before, "a refused native GPU drain must leave shutdown retryable");
					Require(ImGuiApi::GetCurrentContext() == context && allocations.m_liveAllocations > 0 &&
						!probe.m_bWasModuleUnloaded && !probe.m_bWasComponentDestroyedWithContext,
						"failed shutdown must retain the context, workspace component and module");
				}
				if (&probe == &probes.back())
				{
					auto device = VulkanApi::GetInstance()->GetMainDevice();
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), VK_ERROR_OUT_OF_DEVICE_MEMORY);
					const auto before = nativeSubmitAttempts;
					ImGuiFontProbe::Reinitialize();
					Require(nativeSubmitAttempts == before + 1 && !ImGuiFontProbe::Backend()->FontTexture && ImGui::GetIO().Fonts->TexID == 0,
						"partial backend setup must exercise an actual refused font upload before teardown");
				}
				Require(SailorProtocolStopLocalHost(true) != 0 && !App::GetInstance(), "shutdown retry must complete");
				std::cout << "ImGui workspace: frames=" << probe.m_frames << ", callbacks=" << probe.m_callbacksFinished
					<< ", copied=" << probe.m_copiedCallbacks << ", borrowed=" << probe.m_borrowedCallbacks
					<< ", module unloaded=" << probe.m_bWasModuleUnloaded << ", allocations=" << allocations.m_liveAllocations << '\n';
				Require(probe.m_bIsCallbackDataValid && probe.m_callbacksStarted == probe.m_callbacksFinished &&
					probe.m_copiedCallbacks > 0 && probe.m_copiedCallbacks == probe.m_borrowedCallbacks &&
					probe.m_callbacksFinished == probe.m_copiedCallbacks + probe.m_borrowedCallbacks &&
					probe.m_bWasComponentDestroyedAfterCallbacks &&
					probe.m_bWasComponentDestroyedWithContext && probe.m_bWasModuleUnloaded && probe.m_bWasUnloadedAfterContext &&
					!ImGuiApi::GetCurrentContext() && allocations.m_liveAllocations == 0,
					"callbacks, components, context and module must retire in order without leaked ImGui allocations");
			}
			std::cout << "Native ImGui workspace: 24 running sessions, copied/borrowed RHI callbacks, module unload, failed drain/retry and partial backend passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		for (auto& probe : probes)
		{
			probe.m_bIsCallbackReleased = true;
			probe.m_bIsCallbackReleased.notify_all();
		}
		if (SailorProtocolStopLocalHost(true) == 0) result = 1;
		return result;
	}

	int RunMacHostLifetimeGpu(int argc, const char** argv)
	{
		int result = 1;
		try
		{
			std::string initialize;
			for (int i = 0; i < argc; ++i) Tests::ProtocolWire::AppendBytesField(initialize, 1u, argv[i]);
			for (const auto argument : { "--editor", "--port", "0", "--world", "", "--new-world" })
				Tests::ProtocolWire::AppendBytesField(initialize, 1u, argument);
			const auto request = Tests::ProtocolWire::MakeRequest(1u, 10u, initialize);
			Require(ix::initNetSystem(), "host lifetime fixture must initialize local networking");
			const int port = ix::getFreePort();
			Require(ix::uninitNetSystem() && port > 0 && port <= 65535, "host lifetime fixture must reserve a local port");
			constexpr std::string_view token = "0123456789abcdef0123456789abcdef";
			Require(SailorProtocolStartLocalHost(reinterpret_cast<const uint8_t*>(request.data()),
				static_cast<uint32_t>(request.size()), static_cast<uint16_t>(port), token.data(), static_cast<uint32_t>(token.size())) ==
				static_cast<int32_t>(Protocol::EEditorEngineWebSocketHostStatus::Ok), "host lifetime fixture must start the real native protocol host");
			Require(App::IsRendererInitialized() && App::HasEditor(), "host lifetime requires an initialized editor runtime");
			Tests::CheckMacAppHostLifetime();
			Tests::CheckMacAppViewportUpdates();
			Tests::CheckMacHostShutdown([]() { return SailorProtocolStopLocalHost(true) != 0; });
			std::cout << "Native host lifetime test passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		if (SailorProtocolStopLocalHost(true) == 0) result = 1;
		return result;
	}
#endif

	int RunEditorProtocolHost(const std::filesystem::path& directory)
	{
		int result = 1;
		try
		{
			const auto endpoint = YAML::LoadFile((directory / "endpoint.yaml").string());
			const auto port = endpoint["port"].as<uint16_t>();
			const auto token = endpoint["token"].as<std::string>();
			std::ifstream requestFile(directory / "initialize.pb", std::ios::binary);
			const std::string request((std::istreambuf_iterator<char>(requestFile)), {});
			const int32_t status = SailorProtocolStartLocalHost(
				reinterpret_cast<const uint8_t*>(request.data()), static_cast<uint32_t>(request.size()),
				port, token.data(), static_cast<uint32_t>(token.size()));
			{
				std::ofstream ready(directory / "ready.tmp");
				ready << status;
			}
			std::filesystem::rename(directory / "ready.tmp", directory / "ready");
			Require(status == static_cast<int32_t>(Protocol::EEditorEngineWebSocketHostStatus::Ok),
				"the managed integration fixture must start the real editor protocol host");
			Require(App::IsRendererInitialized() && App::HasEditor(), "the protocol host needs a real initialized engine");
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
			while (!std::filesystem::exists(directory / "stop") && std::chrono::steady_clock::now() < deadline)
			{
				// The UI loop outlives App when EngineService sends Shutdown over the socket.
#if defined(__APPLE__)
				Win32::Window::ProcessMacMsgs();
#elif defined(_WIN32)
				Win32::Window::ProcessWin32Msgs();
#endif
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Require(std::filesystem::exists(directory / "stop"), "the managed integration fixture did not stop its host");
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		if (SailorProtocolStopLocalHost(true) == 0) result = 1;
		if (result == 0) std::cout << "Managed editor protocol host completed\n";
		return result;
	}

#if defined(__APPLE__)
	void WriteEditorReadbackGraph(const std::filesystem::path& path, size_t firstTarget,
		glm::ivec2 extent, bool authored = false, bool surface = false, bool unsupportedFirst = false, bool multiple = false)
	{
		constexpr const char* names[] = { "EditorOutput", "Main", "BackBuffer", "Secondary" };
		constexpr const char* colors[] = { "1, 0, 0, 1", "0, 1, 0, 1", "0, 0, 1, 1", "1, 1, 1, 1" };
		std::ofstream graph(path);
		graph << "renderTargets:\n";
		for (size_t i = firstTarget; i < std::size(names); ++i)
		{
			graph << "- name: " << names[i] << "\n  format: " << (unsupportedFirst && i == 0 ? "R32_SFLOAT" : "B8G8R8A8_UNORM") <<
				"\n  width: " << extent.x <<
				"\n  height: " << extent.y << "\n  bIsSurface: " << (surface ? "true" : "false") << '\n';
		}
		graph << "frame:\n";
		for (size_t i = firstTarget; i < std::size(names); ++i)
		{
			graph << "- name: Clear\n  vec4:\n  - clearColor: [" << colors[i] <<
				"]\n  renderTargets:\n  - target: " << names[i] << '\n';
		}
		if (authored)
			graph << "- name: EditorReadback\n  tag: CaptureForInspector\n  renderTargets:\n  - src: Secondary\n";
		if (multiple)
			graph << "- name: EditorReadback\n  renderTargets:\n  - src: Main\n";
		graph.close();
		Require(static_cast<bool>(graph), "temporary editor graph must be written");
	}

	void TestEditorReadbackGraph(const std::filesystem::path& path)
	{
		auto* renderer = App::GetSubmodule<Renderer>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		auto world = App::GetSubmodule<EngineLoop>()->CreateEmptyWorld("Readback graph", static_cast<uint8_t>(EWorldBehaviourBit::EcsTickable));
		uint64_t tick = 0;
		auto pushFrame = [&]()
			{
				Sailor::FrameState frame(world.GetRawPtr(), static_cast<int64_t>(++tick * 16u), {}, { 32, 24 });
				world->Tick(frame);
				Require(renderer->PushFrame(frame), "actual renderer must accept the readback graph frame");
				scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
				scheduler->ProcessTasksOnMainThread();
			};
		ReadbackFramePtr previousGeneration;
		for (size_t scenario = 0; scenario < 11; ++scenario)
		{
			const size_t firstTarget = scenario < 4 ? scenario : 0;
			const bool authored = scenario == 4 || scenario == 9;
			const bool multiple = scenario == 9;
			const bool surface = scenario == 5;
			const glm::ivec2 extent = scenario == 10 ? glm::ivec2(3840, 2160) :
				scenario == 6 ? glm::ivec2(1280, 720) : scenario == 7 ? glm::ivec2(1920, 1080) :
				scenario < 4 ? glm::ivec2(32, 24) : glm::ivec2(67, 39);
			WriteEditorReadbackGraph(path, firstTarget, extent, authored, surface, scenario == 8, multiple);
			const bool appViewport = scenario == 6 || scenario == 7 || scenario == 10;
			if (appViewport)
			{
				App::SetEditorRenderTargetSize(extent.x, extent.y);
				Require(EditorRuntime::ApplyPendingEditorViewportOnEngineThread(), "App viewport sweep must apply each new render extent");
			}
			renderer->RefreshFrameGraph();
			Require(renderer->EnsureFrameGraph() && renderer->HasEditorReadback(),
				"an editor graph without an authored readback must receive an asynchronous producer");
			auto graph = renderer->GetFrameGraph()->GetRHI();
			size_t producers = 0;
			TRefPtr<Framegraph::EditorReadbackNode> node;
			for (auto& candidate : graph->GetGraph())
				if (auto capture = candidate.DynamicCast<Framegraph::EditorReadbackNode>()) { node = capture; ++producers; }
			Require(producers == (multiple ? 2u : 1u) && (multiple || !authored || node->GetTag() == "CaptureForInspector"_h),
				"authored capture tags must be preserved without adding a duplicate producer");
			Require(renderer->EnsureFrameGraph() && graph == renderer->GetFrameGraph()->GetRHI(),
				"repeated EnsureFrameGraph must preserve the active producer and graph");
			if (surface)
				Require(graph->GetSurface("EditorOutput"_h)->NeedsResolve() &&
					graph->GetSurface("EditorOutput"_h)->GetTarget()->GetMsaaSamples() == EMsaaSamples::Samples_4,
					"surface coverage must exercise actual multisampling, not two single-sample aliases");
			if (previousGeneration)
			{
				OnRender([&]() { renderer->QueueEditorReadback(previousGeneration); });
				scheduler->ProcessTasksOnMainThread();
				Require(!renderer->GetEditorReadback(), "a previous graph generation must not republish after replacement");
			}
			for (uint32_t frame = 0; frame < 12u && !renderer->GetEditorReadback(); ++frame) pushFrame();
			EditorRemote::MacRendererFrameSource source;
			Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.m_readback &&
				source.m_readback->m_extent == extent, "actual Render-to-Main publication must expose completed pixels at the current size");
			constexpr uint32_t expected[] = { 0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu };
			const uint32_t color = expected[multiple || scenario == 8 ? 1 : authored ? 3 : firstTarget];
			for (int y = 0; y < extent.y; ++y)
				for (int x = 0; x < extent.x; ++x)
					Require(std::memcmp(source.GetCpuBytes() + y * source.m_bytesPerRow + x * 4, &color, sizeof(color)) == 0,
						"capture must preserve actual pixels and source priority, including resolved surfaces");
			{
				const auto retained = source;
				auto device = VulkanApi::GetInstance()->GetMainDevice();
				FenceDispatchOverride fences(*device);
				SubmitOverride submits(device->GetGraphicsQueue(), VK_ERROR_OUT_OF_HOST_MEMORY);
				const auto beforeSubmits = submitCalls;
				const auto beforeWaits = allFenceWaitCalls;
				const auto start = std::chrono::steady_clock::now();
				for (uint32_t i = 0; i < 1000u; ++i)
					Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.m_readback == retained.m_readback,
						"repeated Main acquisition must retain the same immutable completed frame");
				const auto microseconds = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
				Require(submitCalls == beforeSubmits && allFenceWaitCalls == beforeWaits,
					"Main frame acquisition must neither submit commands nor wait for a GPU fence");
				std::cout << "Readback graph scenario " << scenario << ": 1000 Main acquisitions " << microseconds << " us\n";
			}
			CheckReadbackUpload(source);
			if (appViewport)
			{
				uint32_t nextColor = 0;
				EditorReadbackStats warm, final;
				Tests::CheckMacAppViewportPump(source, [&]()
					{
						++nextColor;
						const uint32_t expectedColor = 0xff000000u | (nextColor * 0x00010101u);
						OnRender([&]() { graph->GetGraph()[0]->SetVec4("clearColor"_h, glm::vec4(glm::vec3(nextColor / 255.0f), 1.0f)); });
						EditorRemote::MacRendererFrameSource next;
						for (uint32_t attempt = 0; attempt < 12; ++attempt)
						{
							pushFrame();
							if (EditorRuntime::TryAcquireEditorReadbackFrameSource(next) &&
								std::memcmp(next.GetCpuBytes(), &expectedColor, 4) == 0) break;
						}
						Require(next.m_readback && std::memcmp(next.GetCpuBytes(), &expectedColor, 4) == 0,
							"new App-frame capture must contain the newly rendered color");
						if (nextColor == 1)
						{
							// Let the producer advance while the consumer still holds its current frame.
							for (uint32_t attempt = 0; attempt < 12 && renderer->GetEditorReadback() == next.m_readback; ++attempt)
								pushFrame();
							const auto unpresented = renderer->GetEditorReadback();
							Require(unpresented && unpresented->m_frameIndex > next.m_readback->m_frameIndex &&
								std::memcmp(unpresented->GetBgraPixels(), &expectedColor, 4) == 0,
								"producer must be able to publish another completed capture before the consumer presents its current one");
						}
						OnRender([&]() { final = node->GetStats(); });
						if (nextColor == 5) warm = final;
						if (nextColor > 5)
							Require(final.m_bufferAllocatedBytes == warm.m_bufferAllocatedBytes &&
								final.m_conversionAllocatedBytes == 0 && final.m_convertedBytes == 0,
								"warm BGRA App presentation must reuse readback buffers without full-frame CPU conversion allocations");
						return next;
					});
				const uint64_t payload = static_cast<uint64_t>(extent.x) * extent.y * 4u;
				Require(final.m_recordedReadbackBytes >= 23u * payload && final.m_bufferAllocatedBytes != 0,
					"App sweep counters must include actual new GPU readbacks and their bounded buffer allocation");
				std::cout << "App readback bytes " << extent.x << 'x' << extent.y << ": allocated " << final.m_bufferAllocatedBytes <<
					", recorded " << final.m_recordedReadbackBytes << ", conversion allocated " << final.m_conversionAllocatedBytes <<
					", converted " << final.m_convertedBytes << '\n';
			}
			if (scenario == 0 || scenario == 5 || scenario == 6)
			{
				EditorReadbackStats before, after;
				OnRender([&]()
					{
						before = node->GetStats();
						if (scenario == 0)
							for (const auto name : { "EditorOutput"_h, "Main"_h, "BackBuffer"_h, "Secondary"_h }) graph->SetRenderTarget(name, {});
						else if (scenario == 5) node->SetRHIResource("src"_h, graph->GetSurface("Main"_h)->GetTarget());
						else node->SetRHIResource_Unresolved("src"_h, "LateOutput"_h);
					});
				// Existing pending captures may still finish; no new capture may be recorded.
				for (uint32_t frame = 0; frame < 4; ++frame) pushFrame();
				const auto last = renderer->GetEditorReadback();
				pushFrame();
				OnRender([&]() { after = node->GetStats(); });
				Require(last && renderer->GetEditorReadback() == last &&
					after.m_recordedReadbackBytes == before.m_recordedReadbackBytes,
					"missing or unresolved sources and raw MSAA images must retain the last completed image without copying");
				if (scenario == 6)
				{
					OnRender([&]() { graph->SetRenderTarget("LateOutput"_h, graph->GetRenderTarget("Secondary"_h)); });
					for (uint32_t frame = 0; frame < 12 && renderer->GetEditorReadback() == last; ++frame) pushFrame();
					Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.m_readback != last,
						"a per-frame source must become readable when its resource is installed");
					const uint32_t white = 0xffffffffu;
					Require(std::memcmp(source.GetCpuBytes(), &white, sizeof(white)) == 0,
						"explicit late source must win over the automatic EditorOutput fallback");
				}
			}
			previousGeneration = source.m_readback;
		}
	}

	int RunEditorReadbackGraphGpu(int argc, const char** argv)
	{
		Tests::TempDirectory workspace("editor-readback-graph");
		std::filesystem::create_directories(workspace.Path("Content"));
		std::string enginePath = std::filesystem::current_path().string();
		for (int i = 1; i + 1 < argc; ++i)
			if (std::string_view(argv[i]) == "--workspace") enginePath = argv[i + 1];
		YAML::Node manifest;
		manifest["manifestVersion"] = 1;
		manifest["workspaceId"] = "00000000-0000-0000-0000-000000000105";
		manifest["name"] = "Readback graph test";
		manifest["enginePath"] = enginePath;
		manifest["engineReferenceKind"] = "source";
		manifest["contentPath"] = "Content";
		manifest["sourcePath"] = "Source";
		manifest["generatedProjectPath"] = "Generated";
		manifest["cachePath"] = "Cache";
		manifest["buildPath"] = "Cache/Build";
		manifest["logicOutputPath"] = "Binaries";
		manifest["logicModuleName"] = "ReadbackGraphTest";
		std::ofstream(workspace.Path("workspace.sailor")) << manifest;
		auto graphics = YAML::LoadFile((std::filesystem::path(enginePath) / "ProjectSettings.yaml").string());
		graphics["graphics"]["defaultQuality"] = "High";
		graphics["graphics"]["presets"]["High"]["msaaSamples"] = 4;
		std::ofstream(workspace.Path("ProjectSettings.yaml")) << graphics;
		const auto graphPath = workspace.Path("Content/EditorRenderer.renderer");
		WriteEditorReadbackGraph(graphPath, 0, { 32, 24 });
		const std::string workspacePath = workspace.Get().string();
		std::vector<const char*> arguments(argv, argv + argc);
		arguments.insert(arguments.end(), { "--workspace", workspacePath.c_str(), "--new-world", "--editor", "--port", "0" });
		App::Initialize(arguments.data(), static_cast<int>(arguments.size()));
		int result = 1;
		try
		{
			Require(App::IsRendererInitialized(), "editor graph test requires an initialized renderer");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			TestEditorReadbackGraph(graphPath);
			std::cout << "Native editor readback graph test passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		if (!App::Shutdown()) result = 1;
		return result;
	}
#endif

	void TestPreparedCursor()
	{
		auto* imGui = App::GetSubmodule<ImGuiApi>();
		auto& io = ImGui::GetIO();
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		std::atomic<uint32_t> reads{ 0 };
		std::atomic<bool> bValid{ true };
		std::jthread nativeReader([&](std::stop_token stop)
		{
			while (!stop.stop_requested())
			{
				const auto cursor = ImGuiApi::GetRequestedMouseCursor();
				if (cursor && (*cursor < ImGuiMouseCursor_None || *cursor >= ImGuiMouseCursor_COUNT)) bValid = false;
				reads.fetch_add(1, std::memory_order_relaxed);
				std::this_thread::yield();
			}
		});
		const auto prepare = [&](ImGuiMouseCursor cursor)
		{
			imGui->NewFrame();
			ImGui::SetMouseCursor(cursor);
			auto command = driver->CreateCommandList(false, ECommandListQueue::Transfer);
			commands->BeginCommandList(command, true);
			const auto frame = imGui->PrepareFrame(command);
			commands->EndCommandList(command);
			Require(frame && driver->SubmitCommandList_Immediate(command), "cursor publication must accompany a real prepared frame");
		};
		for (int cursor = ImGuiMouseCursor_None; cursor < ImGuiMouseCursor_COUNT; ++cursor)
		{
			prepare(cursor);
			Require(ImGuiApi::GetRequestedMouseCursor() == cursor, "every prepared cursor shape must survive the UI handoff");
#if defined(_WIN32)
			if (cursor == ImGuiMouseCursor_TextInput || cursor == ImGuiMouseCursor_Hand || cursor == ImGuiMouseCursor_None)
			{
				SetCursor(LoadCursor(nullptr, IDC_ARROW));
				const auto window = App::GetMainWindow()->GetHWND();
				Require(SendMessage(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)) == TRUE,
					"the native client cursor message must consume the prepared shape");
				const auto expected = cursor == ImGuiMouseCursor_None ? nullptr
					: LoadCursor(nullptr, cursor == ImGuiMouseCursor_Hand ? IDC_HAND : IDC_IBEAM);
				Require(GetCursor() == expected, "WM_SETCURSOR must restore the requested shape instead of the class arrow");
			}
#endif
		}
		io.MouseDrawCursor = true;
		prepare(ImGuiMouseCursor_Hand);
		Require(ImGuiApi::GetRequestedMouseCursor() == ImGuiMouseCursor_None, "a software cursor must hide the native cursor");
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		prepare(ImGuiMouseCursor_TextInput);
		Require(!ImGuiApi::GetRequestedMouseCursor(), "NoMouseCursorChange must relinquish the native cursor even in software mode");
		io.MouseDrawCursor = false;
		io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
		prepare(ImGuiMouseCursor_Arrow);
		nativeReader.request_stop();
		nativeReader.join();
		Require(bValid && reads != 0, "the UI may read cursor snapshots while the frame owner prepares ImGui");
		std::cout << "Input cursor: prepared shapes, software hiding, native override and concurrent UI reads passed\n";
	}

	void TestImGuiModifierSides(ImGuiApi& imGui)
	{
		using Win32::GlobalInput;
		using Type = Platform::InputEvent::Type;
		auto& io = ImGui::GetIO();
		const bool bOriginalMacBehaviors = io.ConfigMacOSXBehaviors;
		struct Modifier { uint32_t key[2]; ImGuiKey physical[2]; ImGuiKey aggregate; };
		for (bool bMacBehaviors : { false, true })
		{
			io.ConfigMacOSXBehaviors = bMacBehaviors;
			Modifier modifiers[] = {
				{ { VK_LSHIFT, VK_RSHIFT }, { ImGuiKey_LeftShift, ImGuiKey_RightShift }, ImGuiKey_ModShift },
				{ { VK_LCONTROL, VK_RCONTROL }, { ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl }, ImGuiKey_ModCtrl },
				{ { VK_LMENU, VK_RMENU }, { ImGuiKey_LeftAlt, ImGuiKey_RightAlt }, ImGuiKey_ModAlt },
				{ { VK_LWIN, VK_RWIN }, { ImGuiKey_LeftSuper, ImGuiKey_RightSuper }, ImGuiKey_ModSuper }
			};
			if (bMacBehaviors)
			{
				// ImGui remaps Ctrl and Super for Mac shortcuts, including their physical keys.
				std::swap(modifiers[1].physical, modifiers[3].physical);
				std::swap(modifiers[1].aggregate, modifiers[3].aggregate);
			}
			for (const auto& modifier : modifiers)
			{
				for (uint32_t first : { 0u, 1u })
				{
					const uint32_t second = 1u - first;
					GlobalInput::ApplyEvent({ Type::Reset });
					GlobalInput::ApplyEvent({ Type::Focus, 0.0f, 0.0f, 0, -1, true });
					GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, modifier.key[first], -1, true });
					imGui.NewFrame();
					Require(ImGui::IsKeyDown(modifier.physical[first]) && !ImGui::IsKeyDown(modifier.physical[second]) &&
						ImGui::IsKeyDown(modifier.aggregate), "ImGui must receive both the physical key and its aggregate modifier");
					ImGui::EndFrame();
					GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, modifier.key[second], -1, true });
					GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, modifier.key[first], -1, false });
					imGui.NewFrame();
					Require(!ImGui::IsKeyDown(modifier.physical[first]) && ImGui::IsKeyDown(modifier.physical[second]) &&
						ImGui::IsKeyDown(modifier.aggregate), "ImGui must preserve a held modifier when its sibling is released");
					ImGui::EndFrame();
					GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, modifier.key[second], -1, false });
					imGui.NewFrame();
					Require(!ImGui::IsKeyDown(modifier.physical[0]) && !ImGui::IsKeyDown(modifier.physical[1]) &&
						!ImGui::IsKeyDown(modifier.aggregate), "ImGui must release the aggregate with its last physical key");
					ImGui::EndFrame();
				}
			}
			std::cout << "Input modifiers: both sides and release orders passed (Mac shortcuts=" << bMacBehaviors << ")\n";
		}
		io.ConfigMacOSXBehaviors = bOriginalMacBehaviors;
		GlobalInput::ApplyEvent({ Type::Reset });
	}

	void TestInputOwner()
	{
		using Win32::GlobalInput;
		using Type = Platform::InputEvent::Type;
		using EditorRemote::InputKind;
		using EditorRemote::InputModifier;
		ImGui::SetCurrentContext(ImGuiApi::GetCurrentContext());
		auto* imGui = App::GetSubmodule<ImGuiApi>();
		auto& io = ImGui::GetIO();
		io.ConfigInputTrickleEventQueue = false;
		GlobalInput::ProcessPendingEvents(false);
		GlobalInput::Reset();
		GlobalInput::ApplyEvent({ Type::Reset });
		const auto previous = GlobalInput::GetInputState();
		std::jthread producer([]
		{
			GlobalInput::QueueNativeEvent({ Type::Focus, 0.0f, 0.0f, 0, -1, true });
			GlobalInput::QueueNativeEvent({ Type::MouseButton, -12.0f, 33.0f, 0, 0, true });
			GlobalInput::QueueNativeEvent({ Type::Key, 0.0f, 0.0f, 'W', -1, true });
			GlobalInput::QueueNativeEvent({ Type::MouseWheel, 0.5f, 3.0f });
			std::string text = "\xC3\xA9\xE8\x88\xB9";
			GlobalInput::QueueNativeEvent({ Type::Text, 0.0f, 0.0f, 0, -1, false, text });
			text.assign(4096, 'x');
			GlobalInput::QueueNativeEvent({ Type::CharacterUtf16, 0.0f, 0.0f, 0x0416 });
		});
		producer.join();
		Require(!GlobalInput::GetInputState().IsKeyDown('W'), "the native producer must not mutate gameplay or ImGui before Main");
		GlobalInput::ProcessPendingEvents(true);
		auto current = GlobalInput::GetInputState();
		current.TrackForChanges(previous);
		imGui->NewFrame();
		Require(current.IsKeyPressed('W') && current.IsButtonClick(VK_LBUTTON) && ImGui::IsKeyDown(ImGuiKey_W) && io.MouseDown[0],
			"one Main-side delivery must update both gameplay and the live engine ImGui context");
		Require(current.GetButtonPressCursorPos(VK_LBUTTON) == glm::ivec2(-12, 33) && io.MousePos.x == -12 && io.MousePos.y == 33,
			"a button event must carry its position without relying on an earlier move");
		Require(current.GetMouseWheelDelta() == 3.0f && io.MouseWheel == 3.0f && io.MouseWheelH == 0.5f,
			"the same normalized wheel event must reach gameplay and ImGui");
		Require(io.InputQueueCharacters.Size == 3 && io.InputQueueCharacters[0] == 0xE9 &&
			io.InputQueueCharacters[1] == 0x8239 && io.InputQueueCharacters[2] == 0x0416,
			"queued UTF-8 must outlive the producer buffer and agree with decoded native UTF-16 input");
		ImGui::EndFrame();

		GlobalInput::QueueNativeEvent({ Type::Focus });
		GlobalInput::ProcessPendingEvents(true);
		imGui->NewFrame();
		Require(!GlobalInput::GetInputState().IsKeyDown('W') && !GlobalInput::GetInputState().IsButtonDown(VK_LBUTTON) &&
			!ImGui::IsKeyDown(ImGuiKey_W) && !io.MouseDown[0], "focus loss must release both consumers in the same frame");
		ImGui::EndFrame();

		GlobalInput::ApplyEvent({ Type::Focus, 0.0f, 0.0f, 0, -1, true });
		GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, 0x0D, -1, true, {}, true });
		GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, VK_MENU, -1, true });
		imGui->NewFrame();
		Require(GlobalInput::GetInputState().IsKeyDown(0x0D) && ImGui::IsKeyDown(ImGuiKey_KeypadEnter) &&
			!ImGui::IsKeyDown(ImGuiKey_Enter) && io.KeyAlt, "native keypad Enter and modifier mappings must survive normalization");
		ImGui::EndFrame();
		GlobalInput::ApplyEvent({ Type::Reset });

		TestImGuiModifierSides(*imGui);

		App::SetEditorRenderTargetSize(64, 48);
		EditorRuntime::ApplyPendingEditorViewportOnEngineThread();
		Require(App::UpsertEditorRemoteViewport(1, 0, 0, 64, 48, true, true), "remote input fixture must register its applied viewport");
		EditorRuntime::DrainEditorRemoteViewportInputOnEngineThread();
		GlobalInput::ApplyEvent({ Type::Focus, 0.0f, 0.0f, 0, -1, true });
		const auto beforeRemote = GlobalInput::GetInputState();
		bool bWasAccepted = false;
		std::jthread protocol([&]
		{
			bWasAccepted = App::SendEditorRemoteViewportInput(1, static_cast<uint32_t>(InputKind::Key),
				0, 0, 0, 0, 'A', 0, 0, true, true, false) &&
				App::SendEditorRemoteViewportInput(1, static_cast<uint32_t>(InputKind::PointerButton),
				27, 19, 0, 0, 0, 0, static_cast<uint32_t>(InputModifier::MouseLeft), true, true, true) &&
				App::SendEditorRemoteViewportInput(1, static_cast<uint32_t>(InputKind::PointerWheel),
				27, 19, 0,
#if defined(_WIN32)
				240,
#else
				2,
#endif
				0, 0, static_cast<uint32_t>(InputModifier::MouseLeft), false, true, true);
		});
		protocol.join();
		Require(bWasAccepted && !GlobalInput::GetInputState().IsKeyDown('A'), "remote protocol delivery must also wait for Main");
		GlobalInput::QueueNativeEvent({ Type::Focus });
		GlobalInput::ProcessPendingEvents(false);
		EditorRuntime::DrainEditorRemoteViewportInputOnEngineThread();
		current = GlobalInput::GetInputState();
		current.TrackForChanges(beforeRemote);
		imGui->NewFrame();
		Require(current.IsKeyDown('A') && ImGui::IsKeyDown(ImGuiKey_A) && current.IsButtonDown(VK_LBUTTON) && io.MouseDown[0] &&
			io.MousePos.x == 27 && io.MousePos.y == 19 && current.GetButtonPressCursorPos(VK_LBUTTON) == glm::ivec2(27, 19),
			"remote click coordinates and keys must survive hidden-native-window callbacks");
		Require(current.GetMouseWheelDelta() == 2.0f && io.MouseWheel == 2.0f,
			"remote wheel units must agree in the world and ImGui frame on both platforms");
		ImGui::EndFrame();
		Require(App::SendEditorRemoteViewportInput(1, static_cast<uint32_t>(InputKind::Focus),
			0, 0, 0, 0, 0, 0, 0, false, false, false), "remote focus loss must be accepted by the live session");
		EditorRuntime::DrainEditorRemoteViewportInputOnEngineThread();
		imGui->NewFrame();
		Require(!GlobalInput::GetInputState().IsKeyDown('A') && !ImGui::IsKeyDown(ImGuiKey_A) && !io.MouseDown[0],
			"remote focus loss must use the same release contract as native input");
		ImGui::EndFrame();
		Require(App::DestroyEditorRemoteViewport(1), "input fixture must release its viewport");
		EditorRuntime::DrainEditorRemoteViewportInputOnEngineThread();
		TestPreparedCursor();
		std::cout << "Input owner: real ImGui, owned Unicode, native/remote parity, hidden source and focus delivery passed\n";
	}

	void TestRemoteInputSessions()
	{
		using Win32::GlobalInput;
		using EditorRemote::InputKind;
		using EditorRemote::InputModifier;
		constexpr uint64_t A = 41, B = 42;
		auto* imGui = App::GetSubmodule<ImGuiApi>();
		auto& io = ImGui::GetIO();
		io.ConfigInputTrickleEventQueue = false;
		const auto upsert = [](uint64_t id, uint32_t width = 64, bool bFocused = false)
		{
			App::SetEditorRenderTargetSize(width, 48);
			EditorRuntime::ApplyPendingEditorViewportOnEngineThread();
			Require(App::UpsertEditorRemoteViewport(id, 0, 0, width, 48, true, bFocused), "input fixture must register the applied viewport");
		};
		const auto send = [](uint64_t id, InputKind kind, uint32_t key = 0, bool bPressed = false,
			bool bFocused = false, bool bCaptured = false, InputModifier modifiers = InputModifier::None,
			float x = 27, float y = 19)
		{
			Require(App::SendEditorRemoteViewportInput(id, static_cast<uint32_t>(kind), x, y, 0, 0,
				key, 0, static_cast<uint32_t>(modifiers), bPressed, bFocused, bCaptured), "live session must accept input");
		};
		const auto held = [](uint32_t key, ImGuiKey guiKey)
		{
			return GlobalInput::GetInputState().IsKeyDown(key) && ImGui::IsKeyDown(guiKey);
		};
		const auto checkFrame = [&](auto condition, std::string_view message)
		{
			EditorRuntime::DrainEditorRemoteViewportInputOnEngineThread();
			imGui->NewFrame();
			const bool bPassed = condition();
			ImGui::EndFrame();
			Require(bPassed, message);
		};
		uint32_t failures = 0;
		const auto run = [&](std::string_view name, auto test)
		{
			upsert(A, 64, true);
			upsert(B);
			send(A, InputKind::Focus, 0, false, true);
			send(A, InputKind::Key, 'W', true);
			send(A, InputKind::PointerButton, 0, true, true, true, InputModifier::MouseLeft);
			checkFrame([&] { return held('W', ImGuiKey_W) && io.MouseDown[0]; }, "fixture must hold W and a mouse button in A");
			try
			{
				test();
				std::cout << "[PASS] Remote input: " << name << '\n';
			}
			catch (const std::exception& error)
			{
				++failures;
				std::cerr << "[FAIL] Remote input: " << name << ": " << error.what() << '\n';
			}
			App::DestroyEditorRemoteViewport(A);
			App::DestroyEditorRemoteViewport(B);
			EditorRuntime::DrainEditorRemoteViewportInputOnEngineThread();
			GlobalInput::ApplyEvent({ Platform::InputEvent::Type::Reset });
		};
		run("inactive destroy", [&]
		{
			Require(App::DestroyEditorRemoteViewport(B), "inactive B must be destroyed");
			checkFrame([&] { return held('W', ImGuiKey_W) && io.MouseDown[0]; }, "destroying B must not release A");
		});
		run("stale packet after accepted input", [&]
		{
			send(A, InputKind::Key, 'D', true);
			send(B, InputKind::Key, 'X', true);
			Require(App::DestroyEditorRemoteViewport(B), "B must invalidate its queued packet");
			checkFrame([&] { return held('W', ImGuiKey_W) && held('D', ImGuiKey_D) &&
				!GlobalInput::GetInputState().IsKeyDown('X') && !ImGui::IsKeyDown(ImGuiKey_X); },
				"a stale packet must be discarded without clearing accepted input");
		});
		run("unrelated focus and capture loss", [&]
		{
			send(B, InputKind::Focus);
			send(B, InputKind::Capture);
			checkFrame([&] { return held('W', ImGuiKey_W) && io.MouseDown[0]; }, "B's release events must not reset A");
		});
		run("focus transfer and late release", [&]
		{
			send(B, InputKind::Focus, 0, false, true);
			send(B, InputKind::Key, 'D', true);
			send(A, InputKind::Focus);
			send(A, InputKind::Capture);
			checkFrame([&] { return held('D', ImGuiKey_D) && !GlobalInput::GetInputState().IsKeyDown('W') &&
				!ImGui::IsKeyDown(ImGuiKey_W) && !io.MouseDown[0]; }, "focus transfer must release A without letting its late events reset B");
		});
		run("unfocused hover", [&]
		{
			send(B, InputKind::PointerMove, 0, false, false, false, InputModifier::None, 91, 73);
			checkFrame([&] { return held('W', ImGuiKey_W) && io.MouseDown[0] &&
				GlobalInput::GetInputState().GetCursorPos() == glm::ivec2(27, 19) && io.MousePos.x == 27 && io.MousePos.y == 19; },
				"hovering an unfocused viewport must not take over active input");
		});
		run("inactive resize", [&]
		{
			upsert(B, 80);
			checkFrame([&] { return held('W', ImGuiKey_W) && io.MouseDown[0]; }, "resizing B must not reset A");
		});
		run("current resize and new generation", [&]
		{
			send(A, InputKind::Key, 'X', true);
			upsert(A, 80, true);
			send(A, InputKind::Key, 'D', true);
			checkFrame([&] { return held('D', ImGuiKey_D) && !GlobalInput::GetInputState().IsKeyDown('W') &&
				!GlobalInput::GetInputState().IsKeyDown('X') && !ImGui::IsKeyDown(ImGuiKey_W) && !ImGui::IsKeyDown(ImGuiKey_X); },
				"resize must discard old-generation input and preserve the new keydown");
		});
		run("same-ID replacement", [&]
		{
			send(A, InputKind::Key, 'X', true);
			Require(App::DestroyEditorRemoteViewport(A), "old A must be destroyed");
			upsert(A, 64, true);
			send(A, InputKind::Key, 'D', true);
			checkFrame([&] { return held('D', ImGuiKey_D) && !GlobalInput::GetInputState().IsKeyDown('X') &&
				!ImGui::IsKeyDown(ImGuiKey_X); }, "reusing a viewport ID must not admit the old session's packet");
		});
		run("ordered reset and renewed focus", [&]
		{
			send(A, InputKind::Key, 'D', true);
			upsert(A, 64, false);
			send(A, InputKind::Focus, 0, false, true);
			send(A, InputKind::Key, 'E', true);
			checkFrame([&] { return held('E', ImGuiKey_E) && !GlobalInput::GetInputState().IsKeyDown('D') &&
				!ImGui::IsKeyDown(ImGuiKey_D); }, "a reset must run between earlier input and renewed focus, not before the whole batch");
		});
		run("owner destroy without new input", [&]
		{
			Require(App::DestroyEditorRemoteViewport(A), "active A must be destroyed");
			checkFrame([&] { return !GlobalInput::GetInputState().IsKeyDown('W') && !ImGui::IsKeyDown(ImGuiKey_W) &&
				!GlobalInput::GetInputState().IsButtonDown(VK_LBUTTON) && !io.MouseDown[0]; }, "owner invalidation must release input even without a new packet");
		});
		for (InputKind release : { InputKind::Focus, InputKind::Capture })
			run(release == InputKind::Focus ? "owner focus loss" : "owner capture loss", [&]
			{
				send(A, release);
				checkFrame([&] { return !GlobalInput::GetInputState().IsKeyDown('W') && !ImGui::IsKeyDown(ImGuiKey_W) &&
					!GlobalInput::GetInputState().IsButtonDown(VK_LBUTTON) && !io.MouseDown[0]; }, "the actual owner's loss must release both consumers");
			});
		run("authoritative modifier state", [&]
		{
			send(A, InputKind::Key, VK_SHIFT, true);
			checkFrame([&] { return held(VK_SHIFT, ImGuiKey_ModShift); }, "explicit modifier keydown must reach both consumers");
			send(A, InputKind::PointerMove, 0, false, false, true, InputModifier::MouseLeft);
			checkFrame([&] { return !GlobalInput::GetInputState().IsKeyDown(VK_SHIFT) && !io.KeyShift && held('W', ImGuiKey_W); },
				"pointer modifiers must reconcile actual key state, not a second stale cache");
		});
		run("physical modifier synchronization", [&]
		{
			constexpr uint32_t keys[] = { VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };
			constexpr ImGuiKey guiKeys[] = { ImGuiKey_LeftShift, ImGuiKey_RightShift, ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl,
				ImGuiKey_LeftAlt, ImGuiKey_RightAlt, ImGuiKey_LeftSuper, ImGuiKey_RightSuper };
			for (auto key : keys)
			{
				send(A, InputKind::Key, key, true);
				checkFrame([&] { return GlobalInput::GetInputState().IsKeyDown(key); }, "physical modifier must reach gameplay");
				send(A, InputKind::PointerMove, 0, false, false, true, InputModifier::MouseLeft);
				checkFrame([&]
				{
					return std::none_of(std::begin(keys), std::end(keys), [&](auto code) { return GlobalInput::GetInputState().IsKeyDown(code); }) &&
						std::none_of(std::begin(guiKeys), std::end(guiKeys), [](auto code) { return ImGui::IsKeyDown(code); }) &&
						!io.KeyShift && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && held('W', ImGuiKey_W);
				}, "an authoritative modifier release must clear physical and aggregate state together");
			}
		});
		run("concurrent input during resize and replacement", [&]
		{
			std::atomic<uint32_t> accepted{ 0 };
			std::atomic<uint32_t> credits{ 0 }, attempts{ 0 };
			std::jthread producer([&](std::stop_token stop)
			{
				bool bPressed = false;
				while (!stop.stop_requested())
				{
					if (credits == 0)
					{
						std::this_thread::yield();
						continue;
					}
					--credits;
					bPressed = !bPressed;
					if (App::SendEditorRemoteViewportInput(A, static_cast<uint32_t>(InputKind::Key),
						0, 0, 0, 0, 'X', 0, 0, bPressed, false, false)) ++accepted;
					App::SendEditorRemoteViewportInput(B, static_cast<uint32_t>(InputKind::Key),
						0, 0, 0, 0, 'Y', 0, 0, bPressed, false, false);
					++attempts;
					attempts.notify_one();
				}
			});
			for (uint32_t iteration = 0; iteration < 16; ++iteration)
			{
				// Issue a bounded burst for each lifecycle transition.
				const auto previousAttempts = attempts.load();
				credits += 128;
				attempts.wait(previousAttempts);
				if ((iteration % 2) == 0) Require(App::DestroyEditorRemoteViewport(A), "concurrent source must release its old session");
				upsert(A, (iteration % 2) == 0 ? 64 : 80, true);
				send(A, InputKind::Focus, 0, false, true);
				send(A, InputKind::Key, 'D', true);
				checkFrame([&] { return held('D', ImGuiKey_D) && !GlobalInput::GetInputState().IsKeyDown('Y') &&
					!ImGui::IsKeyDown(ImGuiKey_Y); }, "current input must survive concurrent producers and reject inactive session keys");
			}
			producer.request_stop();
			producer.join();
			Require(accepted != 0, "the concurrent producer must publish actual session input");
			std::cout << "Remote input concurrent publications: " << accepted << '\n';
			send(A, InputKind::Focus);
			checkFrame([&] { return !GlobalInput::GetInputState().IsKeyDown('D') && !GlobalInput::GetInputState().IsKeyDown('X') &&
				!ImGui::IsKeyDown(ImGuiKey_D) && !ImGui::IsKeyDown(ImGuiKey_X); }, "focus loss must release the final producer state");
		});
		Require(failures == 0, "remote input session ownership regressions failed");
	}

	class SubmissionHistoryMaterial final : public RHIMaterial
	{
	public:
		SubmissionHistoryMaterial() : RHIMaterial(RenderState{}, {}, {}) {}
		size_t GetHistorySize() const
		{
			m_versionLock.Lock();
			const auto count = m_publishedVersions.Num();
			m_versionLock.Unlock();
			return count;
		}
	};

	class SubmissionObservedResources final : public RHIFrameGraphSubmissionResource
	{
	public:
		void ResetForSubmission() override { ++m_numResets; }
		void InvalidateSubmission() override { ++m_numInvalidations; }
		std::atomic<uint32_t> m_numResets{ 0 }, m_numInvalidations{ 0 };
	};

	thread_local VkFence refusedRendererAcquire = VK_NULL_HANDLE;
	thread_local SubmissionHistoryMaterial* acquireMaterial = nullptr;
	thread_local size_t acquireHistorySize = 0;
	thread_local uint32_t refusedAcquires = 0;
	VKAPI_ATTR VkResult VKAPI_CALL RefuseRendererAcquire(VkDevice device, uint32_t count, const VkFence* fences,
		VkBool32 all, uint64_t timeout)
	{
		if (count == 1 && fences[0] == refusedRendererAcquire)
		{
			const auto actual = vkWaitForFences(device, count, fences, all, 5000000000ull);
			if (actual != VK_SUCCESS) return actual;
			acquireMaterial->SetBindings(RHIShaderBindingSetPtr::Make());
			acquireHistorySize = acquireMaterial->GetHistorySize();
			++refusedAcquires;
			return VK_TIMEOUT;
		}
		return vkWaitForFences(device, count, fences, all, timeout);
	}

	class SubmissionLifecycleNode final : public Framegraph::RHINodeDefault
	{
	public:
		Tasks::TaskPtr<> Prepare(RHIFrameGraphPtr, RHISceneViewSnapshot& snapshot) override
		{
			m_snapshot = &snapshot;
			m_view = RendererSubmissionTestAccess::View(*App::GetSubmodule<Renderer>(),
				App::GetSubmodule<EngineLoop>()->GetWorld().GetRawPtr(), snapshot);
			Require(m_view.IsValid(), "the observer must find the actual submission view");
			m_token = m_view->GetOrCreateSubmissionCompletionToken();
			m_completion = snapshot.m_submissionContext->GetOrCreateFrameCompletion();
			m_resources = snapshot.m_submissionContext->GetOrAddFrameGraphResources<SubmissionObservedResources>(
				this, snapshot.m_cameraIndex, 0);
			m_previousMotion = snapshot.m_previousMotionFrame;
			m_generation = snapshot.m_submissionContext->GetResourceGeneration();
			m_version = m_material->GetVersion();
			m_material->SetBindings(RHIShaderBindingSetPtr::Make());
			return Tasks::CreateTask("Prepare retained submission snapshot"_h, [this, &snapshot]()
				{
					m_bStarted.store(true);
					m_bStarted.notify_one();
					m_bReleased.wait(false);
					m_bPrepared = snapshot.m_submissionContext &&
						m_material->GetVersionForSubmission(snapshot.m_submissionContext->GetSubmissionId()) == m_version;
				}, EThreadType::Worker);
		}

		void Process(RHIFrameGraphPtr, RHICommandListPtr transfer, RHICommandListPtr command, const RHISceneViewSnapshot& snapshot) override
		{
			++m_numProcessed;
			m_transferCommand = transfer;
			m_bCapturedVersionMatches = m_material->GetVersionForSubmission(
				snapshot.m_submissionContext->GetSubmissionId()) == m_version;
			if (m_readback)
			{
				m_recordedCommand = command;
				if (m_gpuGate != VK_NULL_HANDLE)
				{
					vkCmdWaitEvents(*command->m_vulkan.m_commandBuffer, 1, &m_gpuGate,
						VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
				}
				command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
				Renderer::GetDriverCommands()->UpdateBuffer(command, m_readback, &m_payload, sizeof(m_payload));
				command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
			}
		}

		TRefPtr<SubmissionHistoryMaterial> m_material;
		RHIMaterialVersionPtr m_version;
		const RHISceneViewSnapshot* m_snapshot = nullptr;
		RHIFencePtr m_completion;
		RHISceneViewPtr m_view;
		RHISubmissionCompletionTokenPtr m_token;
		TRefPtr<SubmissionObservedResources> m_resources;
		TSharedPtr<const RHIMotionHistoryFrame> m_previousMotion;
		RHIBufferPtr m_readback;
		VkEvent m_gpuGate = VK_NULL_HANDLE;
		RHICommandListPtr m_recordedCommand;
		RHICommandListPtr m_transferCommand;
		uint64_t m_generation = 0;
		uint32_t m_payload = 0;
		std::atomic<bool> m_bStarted{ false }, m_bReleased{ false };
		bool m_bPrepared = false;
		bool m_bCapturedVersionMatches = false;
		uint32_t m_numProcessed = 0;
	};

	void TestRendererSubmissionOwnership()
	{
		auto* renderer = App::GetSubmodule<Renderer>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		auto* engine = App::GetSubmodule<EngineLoop>();
		Require(renderer->EnsureFrameGraph(), "submission ownership requires the real graph");
		auto graph = renderer->GetFrameGraph()->GetRHI();
		const auto nodes = graph->GetGraph();
		auto world = engine->GetWorld();
		auto node = TRefPtr<SubmissionLifecycleNode>::Make();
		node->SetTag("SubmissionLifecycle"_h);
		node->m_material = TRefPtr<SubmissionHistoryMaterial>::Make();
		node->m_material->SetBindings(RHIShaderBindingSetPtr::Make());
		Tests::ScopeExit cleanup([&]()
			{
				node->m_bReleased.store(true);
				node->m_bReleased.notify_one();
				renderer->WaitIdle();
				graph->GetGraph() = nodes;
				renderer->RemoveSceneView(world.GetRawPtr());
			});
		graph->GetGraph().Clear();
		graph->GetGraph().Add(node);
		FrameState frame(world.GetRawPtr(), 16, {}, { 32, 24 });
		engine->ProcessCpuFrame(frame);
		frame.GetDrawImGuiTask()->Wait();
		Require(renderer->PushFrame(frame), "real PushFrame must accept the observed submission");
		node->m_bStarted.wait(false);
		Require(!node->m_previousMotion, "a graph without MotionBlur must not prepare temporal scene data");
		Require(node->m_numProcessed == 0 && node->m_material->GetHistorySize() == 2,
			"pending preparation must retain the captured material version and delay recording");
		auto borrowed = renderer->GetOrAddSceneView(world.GetRawPtr());
		Require(borrowed->m_snapshots.IsEmpty() || &borrowed->m_snapshots[0] != node->m_snapshot,
			"an active preparation snapshot must not return to the scene-view cache");
		node->m_bReleased.store(true);
		node->m_bReleased.notify_one();
		scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
		scheduler->ProcessTasksOnMainThread();
		Require(node->m_bPrepared && node->m_bCapturedVersionMatches && node->m_numProcessed == 1,
			"preparation and recording must observe the same captured material exactly once");
		Require(node->m_completion && node->m_completion->Wait(5000000000ull) == EFenceStatus::Finished,
			"the observed submission must complete actual GPU work");
		Require(node->m_material->GetHistorySize() == 1,
			"material capture must close at execution end while the completed render task is still retained");
		auto recycled = renderer->GetOrAddSceneView(world.GetRawPtr());
		Require(!recycled->m_snapshots.IsEmpty() && &recycled->m_snapshots[0] == node->m_snapshot &&
			!recycled->m_submissionContext,
			"completion must clear and return the original view after its borrowers finish");
		std::cout << "Renderer submission: held preparation, captured material revision, one record, native completion and scene-view return passed\n";
	}

	void TestRendererPendingFlightReuse()
	{
		auto* renderer = App::GetSubmodule<Renderer>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		auto* engine = App::GetSubmodule<EngineLoop>();
		auto& driver = Renderer::GetDriver();
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		renderer->WaitIdle();
		Require(renderer->EnsureFrameGraph(), "pending renderer flight requires a real graph");
		auto graph = renderer->GetFrameGraph()->GetRHI();
		const auto nodes = graph->GetGraph();
		auto world = engine->GetWorld();
		auto node = TRefPtr<SubmissionLifecycleNode>::Make();
		node->m_material = TRefPtr<SubmissionHistoryMaterial>::Make();
		node->m_material->SetBindings(RHIShaderBindingSetPtr::Make());
		node->m_bReleased.store(true);
		const VkEventCreateInfo eventInfo{ VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
		Require(vkCreateEvent(*device, &eventInfo, nullptr, &node->m_gpuGate) == VK_SUCCESS,
			"renderer flight test requires a host-signalled GPU event");
		std::atomic<bool> bWaitStarted{ false }, bPushReturned{ false };
		PFN_vkGetFenceStatus status = vkGetFenceStatus;
		PFN_vkWaitForFences wait = ObservePendingFlightWait;
		const bool bWasOutdated = VulkanSubmissionTestAccess::ExchangeSwapchainOutdated(*device, true);
		OnRender([&]()
			{
				pendingFlightWaitStarted = &bWaitStarted;
				VulkanSubmissionTestAccess::ExchangeFenceDispatch(*device, status, wait);
			});
		Tests::ScopeExit cleanup([&]()
			{
				vkSetEvent(*device, node->m_gpuGate);
				renderer->WaitIdle();
				OnRender([&]()
					{
						VulkanSubmissionTestAccess::ExchangeFenceDispatch(*device, status, wait);
						pendingFlightFence = VK_NULL_HANDLE;
						pendingFlightWaitStarted = nullptr;
					});
				VulkanSubmissionTestAccess::ExchangeSwapchainOutdated(*device, bWasOutdated);
				graph->GetGraph() = nodes;
				renderer->RemoveSceneView(world.GetRawPtr());
				vkDestroyEvent(*device, node->m_gpuGate, nullptr);
				node->m_gpuGate = VK_NULL_HANDLE;
			});
		graph->GetGraph().Clear();
		graph->GetGraph().Add(node);
		TVector<RHIBufferPtr> readbacks;
		TVector<RHIFencePtr> completions;
		TRefPtr<SubmissionObservedResources> firstResources;
		for (uint32_t i = 0; i <= driver->GetMaxFramesInFlight(); ++i)
		{
			readbacks.Add(driver->CreateBuffer(sizeof(uint32_t), EBufferUsageBit::BufferTransferDst_Bit,
				EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent));
		}
		for (uint32_t i = 0; i < driver->GetMaxFramesInFlight(); ++i)
		{
			node->m_readback = readbacks[i];
			node->m_payload = 2200u + i;
			FrameState frame(world.GetRawPtr(), 16, {}, { 32, 24 });
			engine->ProcessCpuFrame(frame);
			frame.GetDrawImGuiTask()->Wait();
			Require(renderer->PushFrame(frame), "renderer must accept each free native flight");
			scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			scheduler->ProcessTasksOnMainThread();
			completions.Add(node->m_completion);
			if (i == 0)
			{
				firstResources = node->m_resources;
			}
		}
		const auto firstFence = completions[0]->m_vulkan.m_fence;
		Require(vkGetFenceStatus(*device, *firstFence) == VK_NOT_READY,
			"renderer reuse must be checked before its actual GPU flight completes");
		const uint32_t resetCount = firstResources->m_numResets.load();
		OnRender([&]() { pendingFlightFence = *firstFence; });
		node->m_readback = readbacks[readbacks.Num() - 1u];
		node->m_payload = 2200u + driver->GetMaxFramesInFlight();
		FrameState frame(world.GetRawPtr(), 16, {}, { 32, 24 });
		engine->ProcessCpuFrame(frame);
		frame.GetDrawImGuiTask()->Wait();
		bool bPreservedResources = false;
		VkResult signalResult = VK_ERROR_UNKNOWN;
		std::jthread releaseGpu([&]()
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				while (!bWaitStarted.load() && std::chrono::steady_clock::now() < deadline)
				{
					std::this_thread::yield();
				}
				const VkFence nativeFence = *firstFence;
				bPreservedResources = bWaitStarted.load() &&
					vkWaitForFences(*device, 1, &nativeFence, VK_TRUE, 50000000ull) == VK_TIMEOUT &&
					!bPushReturned.load() && firstResources->m_numResets.load() == resetCount;
				signalResult = vkSetEvent(*device, node->m_gpuGate);
			});
		const bool bAccepted = renderer->PushFrame(frame);
		bPushReturned.store(true);
		releaseGpu.join();
		Require(bPreservedResources && signalResult == VK_SUCCESS && bAccepted &&
			node->m_resources == firstResources && firstResources->m_numResets.load() == resetCount + 1u,
			"Renderer must reset flight resources once, only after their GPU completion");
		scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
		scheduler->ProcessTasksOnMainThread();
		completions.Add(node->m_completion);
		for (size_t i = 0; i < completions.Num(); ++i)
		{
			Require(completions[i]->Wait(5000000000ull) == EFenceStatus::Finished &&
				*static_cast<const uint32_t*>(readbacks[i]->GetPointer()) == 2200u + i,
				"renderer flight reuse must retain each original completion and GPU payload");
		}
		std::cout << "Renderer pending GPU flight: resources retained until fence, one reset on reuse, all payloads passed\n";
	}

	enum class RendererFailure { None, Upload, MainSubmit, Present, GraphRefresh, GraphUpload };
	thread_local RendererFailure rendererFailure = RendererFailure::None;
	thread_local SubmissionLifecycleNode* rendererNode = nullptr;
	thread_local VkCommandBuffer refusedRendererUpload = VK_NULL_HANDLE;
	thread_local VkFence rendererFlightFence = VK_NULL_HANDLE;
	thread_local uint32_t rendererRefusals = 0, rendererPresents = 0, rendererFrameSubmits = 0;

	VKAPI_ATTR VkResult VKAPI_CALL RendererSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence fence)
	{
		bool refuse = rendererFailure == RendererFailure::MainSubmit && fence == rendererFlightFence;
		VkCommandBuffer upload = refusedRendererUpload;
		if (rendererFailure == RendererFailure::GraphUpload && rendererNode->m_transferCommand)
			upload = *rendererNode->m_transferCommand->m_vulkan.m_commandBuffer;
		else if (rendererFailure == RendererFailure::GraphUpload) upload = VK_NULL_HANDLE;
		if (rendererFailure == RendererFailure::Upload || rendererFailure == RendererFailure::GraphUpload)
		{
			for (uint32_t i = 0; i < count; ++i)
			for (uint32_t j = 0; j < info[i].commandBufferCount; ++j)
				refuse |= info[i].pCommandBuffers[j] == upload;
		}
		if (refuse)
		{
			++rendererRefusals;
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}
		const auto result = vkQueueSubmit(queue, count, info, fence);
		if (fence == rendererFlightFence && result == VK_SUCCESS) ++rendererFrameSubmits;
		return result;
	}

	VKAPI_ATTR VkResult VKAPI_CALL RendererPresent(VkQueue queue, const VkPresentInfoKHR* info)
	{
		// Consume the real presentation wait before reporting a recoverable failure.
		const auto result = vkQueuePresentKHR(queue, info);
		++rendererPresents;
		return rendererFailure == RendererFailure::Present &&
			(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) ? VK_ERROR_OUT_OF_DATE_KHR : result;
	}

	class RendererQueueOverride
	{
	public:
		RendererQueueOverride(VulkanDevice& device, RendererFailure failure, VkCommandBuffer upload, SubmissionLifecycleNode* node) :
			m_graphics(device.GetGraphicsQueue()), m_upload(VulkanSubmissionTestAccess::UploadQueue(device, true)),
			m_compute(VulkanSubmissionTestAccess::ComputeQueue(device)), m_present(VulkanSubmissionTestAccess::PresentQueue(device))
		{
			rendererFailure = failure;
			rendererNode = node;
			refusedRendererUpload = upload;
			rendererFlightFence = *VulkanSubmissionTestAccess::FlightFence(device);
			rendererRefusals = rendererPresents = rendererFrameSubmits = 0;
			m_graphicsSubmit = VulkanSubmissionTestAccess::ExchangeSubmit(*m_graphics, RendererSubmit);
			if (m_upload != m_graphics) m_uploadSubmit = VulkanSubmissionTestAccess::ExchangeSubmit(*m_upload, RendererSubmit);
			if (m_compute != m_graphics && m_compute != m_upload)
				m_computeSubmit = VulkanSubmissionTestAccess::ExchangeSubmit(*m_compute, RendererSubmit);
			m_presentFrame = VulkanSubmissionTestAccess::ExchangePresent(*m_present, RendererPresent);
		}
		~RendererQueueOverride()
		{
			VulkanSubmissionTestAccess::ExchangeSubmit(*m_graphics, m_graphicsSubmit);
			if (m_upload != m_graphics) VulkanSubmissionTestAccess::ExchangeSubmit(*m_upload, m_uploadSubmit);
			if (m_compute != m_graphics && m_compute != m_upload)
				VulkanSubmissionTestAccess::ExchangeSubmit(*m_compute, m_computeSubmit);
			VulkanSubmissionTestAccess::ExchangePresent(*m_present, m_presentFrame);
			rendererFailure = RendererFailure::None;
			rendererNode = nullptr;
			rendererFlightFence = VK_NULL_HANDLE;
		}
	private:
		VulkanQueuePtr m_graphics, m_upload, m_compute, m_present;
		PFN_vkQueueSubmit m_graphicsSubmit = nullptr, m_uploadSubmit = nullptr, m_computeSubmit = nullptr;
		PFN_vkQueuePresentKHR m_presentFrame = nullptr;
	};

	void TestRendererSubmissionOutcomes()
	{
		auto* renderer = App::GetSubmodule<Renderer>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		auto* engine = App::GetSubmodule<EngineLoop>();
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto world = engine->GetWorld();
		auto& window = App::GetMainWindow();
		const auto originalExtent = window->GetRenderArea();
		renderer->RefreshFrameGraph();
		Require(renderer->EnsureFrameGraph(), "outcome tests require a fresh real graph");
		auto graph = renderer->GetFrameGraph()->GetRHI();
		auto originalNodes = graph->GetGraph();
		TRefPtr<SubmissionLifecycleNode> node;
		// Keep node identities stable while their flight-local resource keys exist.
		TVector<TRefPtr<SubmissionLifecycleNode>> observers;
		TUniquePtr<RendererQueueOverride> dispatch;
		Tests::ScopeExit cleanup([&]()
			{
				if (node) { node->m_bReleased.store(true); node->m_bReleased.notify_one(); }
				renderer->WaitIdle();
				OnRender([&]() { dispatch.Clear(); });
				graph->GetGraph() = originalNodes;
				window->SetRenderArea(originalExtent);
				renderer->RefreshFrameGraph();
				renderer->EnsureFrameGraph();
				renderer->RemoveSceneView(world.GetRawPtr());
			});
		FrameState previous(world.GetRawPtr(), 0, {}, { 32, 24 });
		auto motionHistory = TRefPtr<Framegraph::MotionBlurNode>::Make();
		motionHistory->SetRHIResource_Unresolved("color"_h, "UnusedMotionOutput"_h);
		const auto run = [&](RendererFailure failure, bool history, bool present = true)
		{
			node = TRefPtr<SubmissionLifecycleNode>::Make();
			observers.Add(node);
			node->SetTag("SubmissionOutcome"_h);
			node->m_material = TRefPtr<SubmissionHistoryMaterial>::Make();
			node->m_material->SetBindings(RHIShaderBindingSetPtr::Make());
			node->m_payload = 0x71a00000u + static_cast<uint32_t>(previous.GetTime());
			node->m_readback = Renderer::GetDriver()->CreateBuffer(sizeof(uint32_t), EBufferUsageBit::BufferTransferDst_Bit,
				EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
			*static_cast<uint32_t*>(node->m_readback->GetPointer()) = 0;
			graph->GetGraph().Clear();
			graph->GetGraph().Add(motionHistory);
			graph->GetGraph().Add(node);
			FrameState frame(world.GetRawPtr(), previous.GetTime() + 16, {}, { 32, 24 }, &previous);
			engine->ProcessCpuFrame(frame);
			frame.GetDrawImGuiTask()->Wait();
			Require(renderer->PushFrame(frame), "Renderer must acquire the observed outcome frame");
			node->m_bStarted.wait(false);
			Require(node->m_previousMotion.IsValid() == history && node->m_token->IsPending(),
				"preparation must see the previous frame's motion result and a pending resource token");
			OnRender([&]()
				{
					dispatch = TUniquePtr<RendererQueueOverride>::Make(*device, failure,
						*frame.GetCommandBuffer(0)->m_vulkan.m_commandBuffer, node.GetRawPtr());
				});
			if (failure == RendererFailure::GraphRefresh) renderer->RefreshFrameGraph();
			node->m_bReleased.store(true);
			node->m_bReleased.notify_one();
			scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			scheduler->ProcessTasksOnMainThread();
			uint32_t refused = 0, presented = 0, submitted = 0;
			OnRender([&]()
				{
					refused = rendererRefusals;
					presented = rendererPresents;
					submitted = rendererFrameSubmits;
					dispatch.Clear();
				});
			const bool uploadFailed = failure == RendererFailure::Upload;
			const bool graphUploadFailed = failure == RendererFailure::GraphUpload;
			const bool mainFailed = failure == RendererFailure::MainSubmit;
			const bool invalidated = uploadFailed || graphUploadFailed || mainFailed || failure == RendererFailure::Present;
			const bool recorded = !uploadFailed && failure != RendererFailure::GraphRefresh;
			Require(refused == uint32_t(uploadFailed || graphUploadFailed || mainFailed) && submitted == uint32_t(!mainFailed) &&
				presented == uint32_t(present && !mainFailed), "the native queue must take exactly the selected outcome");
			Require(node->m_bPrepared && node->m_numProcessed == uint32_t(recorded) &&
				node->m_resources->m_numInvalidations == uint32_t(invalidated),
				"Renderer must record and invalidate each submission exactly once when required");
			Require(!node->m_token->IsPending() && node->m_token->IsSuccessful() == (failure == RendererFailure::None),
				"resource completion must distinguish upload, submit, present and skipped-graph outcomes");
			Require(node->m_material->GetHistorySize() == 1 && !node->m_view->m_submissionContext,
				"every outcome must close material capture and clear the returned view");
			const auto expectedFence = uploadFailed || graphUploadFailed || mainFailed ? EFenceStatus::Failed : EFenceStatus::Finished;
			Require(node->m_completion->Wait(5000000000ull) == expectedFence,
				"frame completion must distinguish refused work from submitted but unpresented work");
			if (mainFailed) Require(Renderer::GetDriver()->FixLostDevice(window.GetRawPtr()), "main-submit refusal must recover real synchronization");
			OnRender([&]() { Require(device->WaitIdle() == VK_SUCCESS, "accepted native work must drain before readback"); });
			Require(*static_cast<const uint32_t*>(node->m_readback->GetPointer()) ==
				(recorded && !mainFailed && !graphUploadFailed ? node->m_payload : 0u), "readback must match work actually submitted by Renderer");
			previous = std::move(frame);
			std::cout << "Renderer outcome=" << static_cast<uint32_t>(failure) << " present=" << present <<
				": exact dispatch, resource token, invalidation, material capture, motion input and GPU payload passed\n";
			return node;
		};

		run(RendererFailure::None, false);
		run(RendererFailure::Upload, true);
		run(RendererFailure::None, false);
		run(RendererFailure::GraphUpload, true);
		run(RendererFailure::None, false);
		run(RendererFailure::MainSubmit, true);
		run(RendererFailure::None, false);
		run(RendererFailure::Present, true);
		// The outdated swapchain still allows an editor frame with no acquired image.
		run(RendererFailure::None, false, false);
		Require(Renderer::GetDriver()->FixLostDevice(window.GetRawPtr()), "present refusal must recover the actual swapchain");
		auto retained = run(RendererFailure::None, true);
		run(RendererFailure::GraphRefresh, true);
		window->SetRenderArea({ 96, 64 });
		Require(renderer->EnsureFrameGraph(), "resize and graph refresh must create a replacement graph");
		auto oldGraph = graph;
		oldGraph->GetGraph() = originalNodes;
		graph = renderer->GetFrameGraph()->GetRHI();
		originalNodes = graph->GetGraph();
		motionHistory = TRefPtr<Framegraph::MotionBlurNode>::Make();
		motionHistory->SetRHIResource_Unresolved("color"_h, "UnusedMotionOutput"_h);
		Require(graph != oldGraph && graph->GetSceneRenderExtent() != oldGraph->GetSceneRenderExtent(),
			"refresh must replace the graph and its render extent");
		auto replacement = run(RendererFailure::None, false);
		Require(replacement->m_generation > retained->m_generation && retained->m_recordedCommand &&
			retained->m_completion->Wait(5000000000ull) == EFenceStatus::Finished &&
			*static_cast<const uint32_t*>(retained->m_readback->GetPointer()) == retained->m_payload,
			"new graph resources must not invalidate retained commands, completion or pixels from an older generation");
		run(RendererFailure::None, true);

		FrameState refusedFrame(world.GetRawPtr(), previous.GetTime() + 16, {}, { 32, 24 }, &previous);
		engine->ProcessCpuFrame(refusedFrame);
		refusedFrame.GetDrawImGuiTask()->Wait();
		PFN_vkGetFenceStatus status = vkGetFenceStatus;
		PFN_vkWaitForFences wait = RefuseRendererAcquire;
		OnRender([&]()
			{
				refusedRendererAcquire = *VulkanSubmissionTestAccess::FlightFence(*device);
				acquireMaterial = node->m_material.GetRawPtr();
				acquireHistorySize = refusedAcquires = 0;
				VulkanSubmissionTestAccess::ExchangeFenceDispatch(*device, status, wait);
			});
		bool accepted = false;
		size_t heldVersions = 0;
		uint32_t refusals = 0;
		{
			Tests::ScopeExit restoreWait([&]()
				{
					OnRender([&]()
						{
							heldVersions = acquireHistorySize;
							refusals = refusedAcquires;
							VulkanSubmissionTestAccess::ExchangeFenceDispatch(*device, status, wait);
							refusedRendererAcquire = VK_NULL_HANDLE;
							acquireMaterial = nullptr;
						});
				});
			accepted = renderer->PushFrame(refusedFrame);
		}
		Require(!accepted && refusals == 1 && heldVersions == 2 && node->m_material->GetHistorySize() == 1,
			"failed acquisition must close the captured material revision before returning");
		auto returned = renderer->GetOrAddSceneView(world.GetRawPtr());
		Require(returned == node->m_view && !returned->m_submissionContext,
			"failed acquisition must return its cleared scene view without recording another frame");
		std::cout << "Renderer acquisition refusal: held material revision, early capture close and cleared view return passed\n";
	}

	class SkyPublicationNode : public Framegraph::SkyNode
	{
	public:
		void Process(RHIFrameGraphPtr, RHICommandListPtr, RHICommandListPtr, const RHISceneViewSnapshot&) override
		{
			parameters = GetSkyParams();
			++frames;
		}

		SkyParameters parameters;
		uint32_t frames = 0;
	};

	void TestSingleActiveWorld()
	{
		ImGui::SetCurrentContext(ImGuiApi::GetCurrentContext());
		auto* engine = App::GetSubmodule<EngineLoop>();
		auto* editor = App::GetSubmodule<Editor>();
		auto& driver = Renderer::GetDriver();
		auto* renderer = App::GetSubmodule<Renderer>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		Require(renderer->EnsureFrameGraph(), "world switching requires the renderer graph");
		auto graph = renderer->GetFrameGraph()->GetRHI();
		const auto originalNodes = graph->GetGraph();
		auto observer = TRefPtr<SkyPublicationNode>::Make();
		observer->SetTag("Sky"_h);
		graph->GetGraph().Clear();
		graph->GetGraph().Add(observer);
		auto active = engine->GetWorld();
		auto candidate = engine->CreateEmptyWorld("Dormant scene candidate", EngineLoop::EditorWorldMask);
		auto activeSky = active->Instantiate("Active sky")->AddComponent<SkyComponent>();
		auto candidateSky = candidate->Instantiate("Dormant sky")->AddComponent<SkyComponent>();
		activeSky->SetCloudsDensity(0.25f);
		candidateSky->SetCloudsDensity(0.75f);
		const auto submit = [&](Sailor::FrameState& frame, SkyParameters expected)
		{
			const auto before = observer->frames;
			Require(renderer->PushFrame(frame), "the renderer must accept the active world's frame");
			scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			scheduler->ProcessTasksOnMainThread();
			OnRender([&]()
			{
				Require(observer->frames == before + 1 && observer->parameters == expected,
					"PushFrame must publish its world's sky before processing that frame's graph");
			});
		};
		const auto activeFrame = active->GetCurrentFrame();
		const auto candidateFrame = candidate->GetCurrentFrame();
		Require(engine->GetWorld() == active && engine->GetWorlds().Num() == 2,
			"creating a candidate must retain the current active world");
		Sailor::FrameState first(active.GetRawPtr(), 16, {}, { 32, 24 });
		engine->ProcessCpuFrame(first);
		first.GetDrawImGuiTask()->Wait();
		Require(active->GetCurrentFrame() == activeFrame + 1 && candidate->GetCurrentFrame() == candidateFrame &&
			!candidate->GetCommandList() && first.GetCommandBuffer(0) == active->GetCommandList(),
			"one frame must tick only the active world and retain its update list, not a dormant candidate's list");
		submit(first, activeSky->GetSkyParameters());
		auto abandoned = engine->CreateEmptyWorld("Abandoned scene candidate", EngineLoop::EditorWorldMask);
		Require(engine->ExitWorld(abandoned.GetRawPtr()), "a dormant candidate must be removable");
		engine->ProcessPendingWorldExits();
		Require(engine->GetWorld() == active && candidate->GetCurrentFrame() == candidateFrame,
			"discarding a dormant candidate must not replace or tick the active world");
		const auto retainedUpdates = first.GetCommandBuffer(0);
		const auto retainedImGui = first.GetDrawImGuiTask();
		editor->SetWorld(candidate.GetRawPtr());
		Require(engine->ExitWorld(active.GetRawPtr()) && engine->GetWorld() == active,
			"requesting an exit must defer retirement until the drain point");
		engine->ProcessPendingWorldExits();
		Require(engine->GetWorld() == candidate && active->GetGameObjects().IsEmpty() &&
			candidate->GetCurrentFrame() == candidateFrame,
			"retirement must clear the old world and promote the candidate without an extra tick");
		App::SetRenderStatsMode(Settings::ERenderStatsMode::RenderStatsAndQueries);
		Sailor::FrameState next(candidate.GetRawPtr(), 32, {}, { 32, 24 });
		engine->ProcessCpuFrame(next);
		next.GetDrawImGuiTask()->Wait();
		Require(candidate->GetCurrentFrame() == candidateFrame + 1 && next.GetCommandBuffer(0) == candidate->GetCommandList() &&
			next.GetCommandBuffer(0) != retainedUpdates && first.GetCommandBuffer(0) == retainedUpdates &&
			first.GetDrawImGuiTask() == retainedImGui,
			"the replacement frame must retain its own updates without changing the previous frame's commands or UI task");
		submit(next, candidateSky->GetSkyParameters());
		App::SetRenderStatsMode(Settings::ERenderStatsMode::None);
		editor->SetWorld(nullptr);
		Require(engine->ExitWorld(candidate.GetRawPtr()), "the last active world must be removable");
		engine->ProcessPendingWorldExits();
		Require(!engine->GetWorld() && engine->GetWorlds().IsEmpty(), "the empty loop must expose no active world");
		Sailor::FrameState empty;
		engine->ProcessCpuFrame(empty);
		empty.GetDrawImGuiTask()->Wait();
		Require(!empty.GetWorld() && !empty.GetCommandBuffer(0) && empty.GetCommandBuffer(1),
			"a frame without a world may prepare UI but must not reuse another world's update commands");
		Require(driver->SubmitCommandList_Immediate(empty.GetCommandBuffer(1)), "the empty frame's UI update must submit");
		graph->GetGraph() = originalNodes;
		std::cout << "Sky PushFrame: active-world parameters precede graph execution and follow deferred world promotion passed\n";
		std::cout << "EngineLoop: one active world, dormant candidates, deferred promotion and retained frame commands passed\n";
	}

	void TestGpuTimingNames()
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		std::array<StringHash, 2> names{ "Independent model-test buffer upload"_h, {} };
		{
			std::string source = "prefix:Submitted GPU scope:suffix";
			names[1] = StringHash::Runtime(std::string_view(source).substr(7, 19));
			source.assign(1024, 'x');
		}
		const std::vector<uint32_t> data(32768, 0x7a4b921eu);
		const size_t size = data.size() * sizeof(uint32_t);
		auto source = driver->CreateBuffer_Immediate(data.data(), size, EBufferUsageBit::BufferTransferSrc_Bit);
		std::array<RHIBufferPtr, 2> readbacks;
		for (auto& readback : readbacks)
		{
			readback = driver->CreateBuffer(size, EBufferUsageBit::BufferTransferDst_Bit,
				EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
			Require(source && readback, "GPU timing fixture must allocate its copy buffers");
		}
		Require(driver->BeginGpuFrameTimeQuery(1) && driver->StartGpuTracking(),
			"GPU timing fixture requires native timestamp queries");
		auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(command, true);
		const auto range = driver->BeginGpuFrameTimeRange(command);
		Require(range != IGraphicsDriver::InvalidGpuFrameTimeRange, "the native frame range must begin");
		auto& native = command->m_vulkan.m_commandBuffer;
		native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		for (size_t i = 0; i < names.size(); ++i)
		{
			const auto query = commands->BeginGpuTimestamp(command, names[i]);
			Require(query != InvalidGpuTimestampQuery, "literal and dynamic timing identifiers must record native queries");
			native->CopyBuffer(*source->m_vulkan.m_buffer->Get(), *readbacks[i]->m_vulkan.m_buffer->Get(), size);
			commands->EndGpuTimestamp(command, query);
		}
		native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
		driver->EndGpuFrameTimeRange(command, range);
		commands->EndCommandList(command);
		driver->FinishGpuTracking();
		driver->EndGpuFrameTimeQuery();
		Require(driver->SubmitCommandList_Immediate(command), "the measured copies must finish on the GPU");
		driver->CommitGpuFrameTimeQuery();
		Require(driver->BeginGpuFrameTimeQuery(1), "the next frame must poll the completed queries");
		const auto result = driver->TakeGpuTimingResult();
		driver->CancelGpuFrameTimeQuery();
		Require(result && result->m_bValid && result->m_timings.Num() == names.size(),
			"native readback must publish both timing scopes");
		for (size_t i = 0; i < names.size(); ++i)
		{
			Require(result->m_timings[i].m_name == names[i] && result->m_timings[i].m_queue == ECommandListQueue::Graphics,
				"native query results must retain their label identity and queue");
			Require(std::equal(data.begin(), data.end(), static_cast<const uint32_t*>(readbacks[i]->GetPointer())),
				"each measured native copy must preserve its buffer contents");
		}
		Require(result->m_timings[0].m_name.ToString() == "Independent model-test buffer upload" &&
			result->m_timings[1].m_name.ToString() == "Submitted GPU scope",
			"native results must retain readable literal and bounded dynamic names after source destruction");
		std::cout << "Native GPU timing names: literal and bounded dynamic labels survived submission and readback\n";
	}

	int RunFenceGpu(int argc, const char** argv, std::string_view mode)
	{
		std::vector<const char*> arguments(argv, argv + argc);
		const bool editorReadback = mode.starts_with("--gpu-editor-readback") || mode.starts_with("--gpu-metal-");
		if (editorReadback) arguments.insert(arguments.end(), { "--editor", "--port", "0" });
		if (mode == "--gpu-imgui-fonts" || mode == "--gpu-engine-loop" || mode == "--gpu-input-owner" ||
			mode == "--gpu-editor-messages" || mode == "--gpu-editor-events" || mode == "--gpu-input-sessions")
			arguments.insert(arguments.end(), { "--editor", "--port", "0", "--world", "", "--new-world" });
		App::Initialize(arguments.data(), static_cast<int>(arguments.size()));
		int result = 1;
		try
		{
			Require(App::IsRendererInitialized(), "fence test requires an initialized renderer");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			if (mode == "--gpu-submission-statistics") TestConcurrentSubmissionStatistics();
			else if (mode == "--gpu-engine-loop")
			{
				TestRendererSubmissionOwnership();
				TestRendererPendingFlightReuse();
				TestRendererSubmissionOutcomes();
				TestSingleActiveWorld();
				Tests::RunAnimationShadowCommandTests();
				Tests::RunWorldLifecycleCommandTests();
			}
			else if (mode == "--gpu-input-owner") TestInputOwner();
			else if (mode == "--gpu-input-sessions") TestRemoteInputSessions();
			else if (mode == "--gpu-editor-messages") Tests::RunEditorMessageViewTests();
			else if (mode == "--gpu-editor-events") Tests::RunEditorViewportCommandTests();
			else if (mode == "--gpu-imgui-fonts") TestImGuiFontInitialization();
			else if (mode == "--gpu-meshes") TestMeshInitialization();
			else if (mode == "--gpu-editor-readback") TestEditorReadback();
			else if (mode == "--gpu-texture-capture") TestTextureCaptures();
#if defined(__APPLE__)
			else if (mode == "--gpu-metal-export") OnRender([]() { TestMetalTextureExport(); });
			else if (mode == "--gpu-metal-retirement") OnRender([]() { TestMetalTextureRetirement(); });
#endif
			else if (editorReadback) TestEditorReadbackRefusal(mode == "--gpu-editor-readback-lost");
			else OnRender([&]()
				{
					if (mode == "--gpu-extended-submit") TestExtendedSubmission(false);
					else if (mode == "--gpu-timing-names") TestGpuTimingNames();
					else if (mode == "--gpu-frame-completion") TestFrameCompletionReuse();
					else if (mode == "--gpu-extended-submit-lost") TestExtendedSubmission(true);
#if defined(_WIN32)
					else if (mode == "--gpu-windows-shared") TestWindowsSharedSurfaceCopy(false);
					else if (mode == "--gpu-windows-shared-lost") TestWindowsSharedSurfaceCopy(true);
#endif
					else if (mode == "--gpu-immediate-image-create") TestImmediateImageCreation(false);
					else if (mode == "--gpu-immediate-images") TestImmediateImageContents();
					else if (mode == "--gpu-textures") TestAsynchronousImagePublication(false);
					else if (mode == "--gpu-cubemaps") TestAsynchronousImagePublication(true);
					else if (mode == "--gpu-cubemap-pending") TestCubemapPending();
					else if (mode == "--gpu-immediate-image-create-lost") TestImmediateImageCreation(true);
					else if (mode == "--gpu-immediate-buffer-create") TestImmediateBufferCreation();
					else if (mode == "--gpu-immediate-buffers")
					{
						TestImmediateBufferCreation();
						TestImmediateBufferCopy(false);
						TestParticleBufferPublication();
					}
					else if (mode == "--gpu-immediate-buffer-create-lost") TestImmediateBufferCreation(true);
					else if (mode == "--gpu-immediate-buffer-copy-lost") TestImmediateBufferCopy(true);
					else if (mode == "--gpu-immediate")
					{
						TestImmediateSubmission(false);
						TestImmediateBindingUpdate(false);
					}
					else if (mode == "--gpu-immediate-lost") TestImmediateSubmission(true);
					else if (mode == "--gpu-immediate-binding-lost") TestImmediateBindingUpdate(true);
					else if (mode == "--gpu-fence-completion") TestFenceCompletionAndReuse();
					else TestAcceptedUploadLoss(mode == "--gpu-fence-wait-loss");
				});
			std::cout << "Native fence test passed: " << mode << '\n';
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		if (!App::Shutdown()) result = 1;
		if (mode == "--gpu-input-owner" && ImGuiApi::GetRequestedMouseCursor())
		{
			std::cerr << "ImGui shutdown must release the native cursor override\n";
			result = 1;
		}
		return result;
	}

	int RunGpu(int argc, const char** argv, bool present, bool lost, bool upload = false, bool accepted = false)
	{
		App::Initialize(argv, argc);
		int result = 1;
		try
		{
			Require(App::IsRendererInitialized(), "GPU test requires an initialized renderer");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				if (upload)
				{
					OnRender([&]() { TestNativeUploadFailure(lost ? VK_ERROR_DEVICE_LOST : error, accepted); });
					if (lost) break;
					continue;
				}
				FailedFrame failed;
				OnRender([&]() { TestNativeFrameFailure(present, lost ? VK_ERROR_DEVICE_LOST : error, failed); });
				if (present && !lost)
				{
					OnRender([&]()
						{
							auto device = VulkanApi::GetInstance()->GetMainDevice();
							const auto before = submitCalls;
							SubmitOverride refusal(device->GetGraphicsQueue(), error);
							Require(!device->FixLostDevice(App::GetMainWindow().GetRawPtr()),
								"failed recovery discarded a pending acquired image");
							Require(submitCalls == before + 1u && lastWait == *failed.acquire &&
								lastSignal == VK_NULL_HANDLE && lastCommandCount == 0u,
								"recovery must consume exactly the outstanding acquisition without signaling a failed flight");
							Require(VulkanSubmissionTestAccess::FlightFence(*device) == failed.fence &&
								failed.frame.command->m_vulkan.m_commandBuffer.NumRefs() == 2u,
								"recovery refusal must retain the old flight and dependencies");
						});
				}
				const bool recovered = Renderer::GetDriver()->FixLostDevice(App::GetMainWindow().GetRawPtr());
				Require(recovered != lost, "only swapchain failure can be recovered without replacing VkDevice");
				if (lost)
				{
					OnRender([&]()
						{
							auto device = VulkanApi::GetInstance()->GetMainDevice();
							Require(VulkanSubmissionTestAccess::FlightFence(*device) == failed.fence &&
								failed.fence->Status() == VK_NOT_READY, "device loss must not replace or signal frame sync objects");
						});
					break;
				}
				OnRender([&]() { TestRecoveredFrames(present, failed); });
				std::cout << "Recovered error " << error << " with verified GPU frame readbacks\n";
			}
			std::cout << "Native frame failure test passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		if (!App::Shutdown()) result = 1;
		return result;
	}
}

namespace Sailor::Tests
{
	void RequireNoTransferSubmission(const std::function<void()>& record)
	{
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
		WorkerUploadOverride observe(VK_SUCCESS, true);
		record();
		scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
		Require(workerSubmitCalls.load() == 0, "a transform-only update must not submit GPU transfers");
	}

	void RequireMainMeshUploadRefusal(const std::function<void()>& record, VkResult error, uint32_t precedingUploads)
	{
		App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
		WorkerUploadOverride refusal(error, true, precedingUploads);
		record();
		Require(workerSubmitRefusals.load() == 1 && !workerSubmitWasOffCaller.load(),
			"the terrain fixture must reject a real mesh transfer on the world caller after its accepted prefix");
	}

	void RequirePendingMeshUpload(const std::function<void()>& load, const std::function<void()>& checkPending)
	{
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		FenceDispatchOverride dispatch(*device);
		{
			WorkerUploadOverride observe(VK_SUCCESS, true);
			load();
			scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			Require(workerSubmitCalls.load() == 1 && workerSubmittedFence.load(),
				"the model must submit its real transfer command without waiting for the GPU");
			observedFences[0] = workerSubmittedFence.load();
		}
		fenceResults[0] = VK_NOT_READY;
		Renderer::GetDriver()->TrackResources_ThreadSafe();
		checkPending();
	}

	void RequireWorkerUploadRefusal(const std::function<void()>& load, VkResult error, bool transfer)
	{
		App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
		WorkerUploadOverride refusal(error, transfer);
		load();
		Require(workerSubmitRefusals.load() == 1 && workerSubmitWasOffCaller.load(),
			"the upload task must reach exactly one native initialization refusal off the calling thread");
	}

	void RequireTexturePixels(RHITexturePtr texture, const std::vector<uint32_t>& expected)
	{
		OnRender([&]()
			{
				Require(ReadImage(*Renderer::GetDriver(), texture->m_vulkan.m_image) == expected,
					"every published texture pixel must match the accepted importer revision");
			});
	}

	void RequireImageInitializationRefusal(const std::function<void()>& record,
		uint32_t precedingSubmits, uint32_t refusals, VkResult error)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error, false, precedingSubmits);
		const auto before = nativeSubmitAttempts;
		record();
		Require(nativeSubmitAttempts == before + precedingSubmits + refusals,
			"the consumer must reach the requested native image initialization refusal");
	}

	void RequireAttachmentInitializationRefusal(const std::function<void()>& record, uint32_t precedingSubmits, VkResult error)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		RenderingDispatchOverride rendering(*device);
		SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error, false, precedingSubmits);
		const auto before = nativeSubmitAttempts;
		record();
		Require(nativeSubmitAttempts == before + precedingSubmits + 1 && nativePassBegins == 0 && nativePassEnds == 0,
			"a refused node attachment must record neither a native pass nor an unmatched end");
	}

	void RequireRejectedComputeSubmission(const std::function<bool()>& submit)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		SubmitOverride rejection(VulkanSubmissionTestAccess::ComputeQueue(*device), VK_ERROR_OUT_OF_HOST_MEMORY);
		const auto before = submitCalls;
		Require(!submit() && submitCalls == before + 1u, "the native compute queue must reject the submission");
	}

	void RequireRejectedGraphicsSubmission(const std::function<bool()>& submit)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		SubmitOverride rejection(VulkanSubmissionTestAccess::UploadQueue(*device, false), VK_ERROR_OUT_OF_HOST_MEMORY);
		const auto before = submitCalls;
		Require(!submit() && submitCalls == before + 1u, "the native graphics queue must reject the submission");
	}

	void RequireRejectedNativeSubmission(RHICommandListPtr command)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		SubmitOverride rejection(VulkanSubmissionTestAccess::UploadQueue(*device, command->IsTransferOnly()), VK_ERROR_OUT_OF_HOST_MEMORY);
		const auto before = submitCalls;
		Require(!Renderer::GetDriver()->SubmitCommandList(command, RHIFencePtr::Make()) && submitCalls == before + 1u,
			"the native queue must reject the recorded command list");
	}
}

int main(int argc, const char** argv)
{
	for (int i = 1; i < argc; ++i)
	{
		const std::string_view mode(argv[i]);
		if (mode == "--gpu-cloud-noise" || mode == "--gpu-cloud-noise-msaa2" || mode == "--gpu-sky-stars")
			return Tests::RunSkyGpu(argc, argv, mode == "--gpu-sky-stars");
		if (mode == "--gpu-msaa-cache") return RunAttachmentGpu(argc, argv, true);
		if (mode == "--gpu-render-targets") return RunAttachmentGpu(argc, argv, false);
		if (mode == "--gpu-editor-protocol-host" && i + 1 < argc) return RunEditorProtocolHost(argv[i + 1]);
#if defined(__APPLE__)
		if (mode == "--gpu-editor-readback-graph") return RunEditorReadbackGraphGpu(argc, argv);
		if (mode == "--gpu-metal-export" || mode == "--gpu-metal-retirement") return RunFenceGpu(argc, argv, mode);
#else
		if (mode == "--gpu-editor-readback-graph" || mode.starts_with("--gpu-metal-")) return 77;
#endif
#if defined(_WIN32)
		if (mode == "--gpu-windows-shared" || mode == "--gpu-windows-shared-lost") return RunFenceGpu(argc, argv, mode);
#else
		if (mode == "--gpu-windows-shared" || mode == "--gpu-windows-shared-lost")
		{
			std::cout << "Windows shared-surface GPU tests require Windows\n";
			return 77;
		}
#endif
		if (mode == "--gpu-bootstrap-submit") return RunBootstrapGpu(argc, argv, false, false);
		if (mode == "--gpu-editor-simulation") return Tests::RunEditorSimulationTests(argc, argv);
		if (mode == "--gpu-render-contracts" || mode == "--gpu-render-contracts-msaa2") return Tests::RunRenderContractsGpu(argc, argv);
#if defined(__APPLE__)
		if (mode == "--gpu-capabilities") return RunBootstrapGpu(argc, argv, false, false, true);
		if (mode == "--gpu-app-bootstrap") return RunAppBootstrapGpu(argc, argv);
		if (mode == "--gpu-landscape") return Tests::RunLandscapeGpu(argc, argv);
		if (mode == "--gpu-pathtracer-khr")
		{
			auto& overrides = Tests::GetVulkanCapabilityOverrides();
			overrides.deviceApiVersion = VK_API_VERSION_1_2;
			overrides.loaderApiVersion = VK_API_VERSION_1_1;
			const int result = Tests::RunPathTracerCommandTests(argc, argv);
			if (result != 0) return result;
			if (!overrides.khrRenderingLookups || overrides.coreRenderingLookups || !overrides.enabledKhrRendering ||
				overrides.instanceTarget != VK_API_VERSION_1_3)
			{
				std::cerr << "Vulkan 1.2 device must render through enabled KHR commands with the application's 1.3 target\n";
				return 1;
			}
			std::cout << "Native Vulkan KHR rendering passed with a simulated 1.1 loader and 1.2 device\n";
			return 0;
		}
#else
		if (mode == "--gpu-capabilities" || mode == "--gpu-app-bootstrap" || mode == "--gpu-pathtracer-khr")
		{
			std::cerr << "This native Vulkan test requires the macOS interposer\n";
			return 77;
		}
#endif
		if (mode == "--gpu-initialization") return RunInitializationGpu(argc, argv);
		if (mode == "--gpu-pathtracer-images" || mode == "--gpu-pathtracer-images-msaa2") return Tests::RunRenderContractsGpu(argc, argv, true);
		if (mode == "--gpu-pathtracer" || mode == "--gpu-pathtracer-1x" || mode == "--gpu-gi-shutdown")
			return Tests::RunPathTracerCommandTests(argc, argv);
		if (mode == "--gpu-bootstrap-submit-lost") return RunBootstrapGpu(argc, argv, false, true);
		if (mode == "--gpu-bootstrap-wait") return RunBootstrapGpu(argc, argv, true, false);
		if (mode == "--gpu-bootstrap-lost") return RunBootstrapGpu(argc, argv, true, true);
		if (mode == "--gpu-shutdown-acquire") return RunShutdownGpu(argc, argv, false);
		if (mode == "--gpu-shutdown-idle") return RunShutdownGpu(argc, argv, true);
		if (mode == "--gpu-host-shutdown-acquire") return RunShutdownGpu(argc, argv, false, true);
		if (mode == "--gpu-host-shutdown-idle") return RunShutdownGpu(argc, argv, true, true);
#if defined(__APPLE__)
		if (mode == "--gpu-mac-host-lifetime") return RunMacHostLifetimeGpu(argc, argv);
		if (mode == "--gpu-imgui-lifetime") return RunImGuiLifetimeGpu(argc, argv);
		if (mode == "--gpu-imgui-workspace") return RunImGuiWorkspaceGpu(argc, argv);
#else
		if (mode == "--gpu-imgui-lifetime" || mode == "--gpu-imgui-workspace") return 77;
#endif
		if (mode == "--gpu-editor-readback" || mode == "--gpu-editor-readback-refused" || mode == "--gpu-editor-readback-lost" ||
			mode == "--gpu-texture-capture" ||
			mode == "--gpu-timing-names" ||
			mode == "--gpu-engine-loop" ||
			mode == "--gpu-input-owner" ||
			mode == "--gpu-input-sessions" ||
			mode == "--gpu-editor-messages" ||
			mode == "--gpu-editor-events" ||
			mode == "--gpu-submission-statistics" ||
			mode == "--gpu-frame-completion" ||
			mode == "--gpu-fence-poll-loss" || mode == "--gpu-fence-wait-loss" || mode == "--gpu-fence-completion" ||
			mode == "--gpu-immediate" || mode == "--gpu-immediate-lost" || mode == "--gpu-immediate-binding-lost" ||
			mode == "--gpu-immediate-buffer-create" || mode == "--gpu-immediate-buffers" ||
			mode == "--gpu-immediate-image-create" || mode == "--gpu-immediate-image-create-lost" ||
			mode == "--gpu-immediate-images" ||
			mode == "--gpu-textures" || mode == "--gpu-cubemaps" || mode == "--gpu-cubemap-pending" ||
			mode == "--gpu-imgui-fonts" || mode == "--gpu-meshes" ||
			mode == "--gpu-extended-submit" || mode == "--gpu-extended-submit-lost" ||
			mode == "--gpu-immediate-buffer-create-lost" || mode == "--gpu-immediate-buffer-copy-lost")
			return RunFenceGpu(argc, argv, mode);
		if (std::string_view(argv[i]) == "--gpu-present") return RunGpu(argc, argv, true, false);
		if (std::string_view(argv[i]) == "--gpu-offscreen") return RunGpu(argc, argv, false, false);
		if (std::string_view(argv[i]) == "--gpu-lost-present") return RunGpu(argc, argv, true, true);
		if (std::string_view(argv[i]) == "--gpu-lost-offscreen") return RunGpu(argc, argv, false, true);
		if (std::string_view(argv[i]) == "--gpu-upload") return RunGpu(argc, argv, false, false, true);
		if (std::string_view(argv[i]) == "--gpu-upload-lost") return RunGpu(argc, argv, false, true, true);
		if (std::string_view(argv[i]) == "--gpu-upload-pending-loss") return RunGpu(argc, argv, false, true, true, true);
	}
	try
	{
		TestQueueDispatch();
		Tests::RunLoggingWithoutAppTests();
		std::cout << "Vulkan submission tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
