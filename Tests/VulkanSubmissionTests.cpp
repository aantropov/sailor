#include "Sailor.h"
#include "Engine/Frame.h"
#include "FrameGraph/ParticlesNode.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "GraphicsDriver/Vulkan/VulkanQueue.h"
#include "GraphicsDriver/Vulkan/VulkanFence.h"
#include "GraphicsDriver/Vulkan/VulkanSemaphore.h"
#include "GraphicsDriver/Vulkan/VulkanSwapchain.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Material.h"
#include "RHI/Renderer.h"
#include "RHI/Texture.h"
#include "Tasks/Tasks.h"

#include <iostream>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

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
		static VulkanFencePtr FlightFence(const VulkanDevice& device) { return device.m_syncFences[device.m_currentFrame]; }
		static VulkanSemaphorePtr AcquireSemaphore(const VulkanDevice& device) { return device.m_imageAvailableSemaphores[device.m_currentFrame]; }
		static VulkanSemaphorePtr PresentSemaphore(const VulkanDevice& device) { return device.m_renderFinishedSemaphores[device.m_currentSwapchainImageIndex]; }
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

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	thread_local VkResult nextResult = VK_SUCCESS;
	thread_local uint32_t submitCalls = 0u;
	thread_local uint32_t lastSubmitCount = 0u;
	thread_local uint32_t lastCommandCount = 0u;
	thread_local bool rejectNativeSubmit = false;
	thread_local bool submitBeforeFailure = false;
	thread_local uint32_t submitsBeforeFailure = 0u;
	thread_local VkSemaphore lastWait = VK_NULL_HANDLE;
	thread_local VkSemaphore lastSignal = VK_NULL_HANDLE;
	PFN_vkQueueSubmit forwardNativeSubmit = nullptr;
	PFN_vkGetFenceStatus forwardFenceStatus = nullptr;
	PFN_vkWaitForFences forwardFenceWait = nullptr;
	thread_local std::array<VkFence, 2> observedFences{};
	thread_local std::array<VkResult, 2> fenceResults{};
	thread_local uint32_t fenceStatusCalls = 0u;
	thread_local uint32_t fenceWaitCalls = 0u;
	thread_local VkResult fenceWaitResult = VK_TIMEOUT;
	thread_local bool captureNextFenceWait = false;
	thread_local bool capturedFenceCompleted = false;
	thread_local uint32_t waitsBeforeCapture = 0u;

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
		if (rejectNativeSubmit && submitsBeforeFailure > 0u)
		{
			--submitsBeforeFailure;
			return forwardNativeSubmit(queue, count, info, fence);
		}
		if (rejectNativeSubmit)
		{
			StubSubmit(queue, count, info, fence);
			lastCommandCount = count ? info[0].commandBufferCount : 0u;
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

	struct RecordedFrame
	{
		RHICommandListPtr command;
		RHIBufferPtr readback;
		std::array<uint32_t, 64> expected;
	};

	RecordedFrame RecordFrame(uint32_t seed)
	{
		RecordedFrame frame;
		for (uint32_t i = 0; i < frame.expected.size(); ++i) frame.expected[i] = seed + 37u * i;
		frame.readback = Renderer::GetDriver()->CreateBuffer(sizeof(frame.expected), EBufferUsageBit::BufferTransferDst_Bit,
			EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
		std::fill_n(static_cast<uint32_t*>(frame.readback->GetPointer()), frame.expected.size(), 0xdeadbeefu);
		frame.command = Renderer::GetDriver()->CreateCommandList(false, ECommandListQueue::Graphics);
		auto commands = Renderer::GetDriverCommands();
		commands->BeginCommandList(frame.command, true);
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
		auto task = Tasks::CreateTaskWithResult<std::string>("Native frame failure validation", [function = std::move(function)]()
			{
				try { function(); return std::string{}; }
				catch (const std::exception& error) { return std::string(error.what()); }
			}, EThreadType::Render);
		task->Run();
		task->Wait();
		const auto error = task->GetResult();
		if (!error.empty()) throw std::runtime_error(error);
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
		driver->AddBufferToShaderBindings(bindings, buffer, "immediate", 0u);
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
		Require(driver->UpdateShaderBinding_Immediate(bindings, "immediate", data.data(), sizeof(data)),
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
					Require(!driver->UpdateShaderBinding_Immediate(bindings, "immediate", refused.data(), sizeof(refused)),
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
				Require(!driver->UpdateShaderBinding_Immediate(bindings, "immediate", data.data(), sizeof(data)) && capturedFenceCompleted,
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
	};

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
					Require(node.m_perInstanceData->GetOrAddShaderBinding("data")->m_vulkan.m_valueBinding == node.m_instances->m_vulkan.m_buffer &&
						node.m_perInstanceData->GetOrAddShaderBinding("particlesData")->m_vulkan.m_valueBinding == node.m_particlesFrames->m_vulkan.m_buffer,
						"particle bindings must retain the published buffers' original allocations");
				}
			}
		}
	}

	int RunFenceGpu(int argc, const char** argv, std::string_view mode)
	{
		App::Initialize(argv, argc);
		int result = 1;
		try
		{
			Require(App::IsRendererInitialized(), "fence test requires an initialized renderer");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			OnRender([&]()
				{
					if (mode == "--gpu-immediate-buffer-create") TestImmediateBufferCreation();
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
		App::Shutdown();
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
		App::Shutdown();
		return result;
	}
}

int main(int argc, const char** argv)
{
	for (int i = 1; i < argc; ++i)
	{
		const std::string_view mode(argv[i]);
		if (mode == "--gpu-fence-poll-loss" || mode == "--gpu-fence-wait-loss" || mode == "--gpu-fence-completion" ||
			mode == "--gpu-immediate" || mode == "--gpu-immediate-lost" || mode == "--gpu-immediate-binding-lost" ||
			mode == "--gpu-immediate-buffer-create" || mode == "--gpu-immediate-buffers" ||
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
		std::cout << "Vulkan submission tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
