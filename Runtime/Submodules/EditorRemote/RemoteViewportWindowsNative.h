#pragma once

#if defined(_WIN32)

#include "RemoteViewportWindowsTransport.h"

#include <string>

namespace Sailor::EditorRemote
{
	class SailorWindowsSharedSurfaceProvider final : public IWindowsSharedSurfaceProvider
	{
	public:
		SailorWindowsSharedSurfaceProvider();
		~SailorWindowsSharedSurfaceProvider() override;

		Failure CreateOrResizeSurface(
			const ViewportDescriptor& viewport,
			ConnectionEpoch epoch,
			SurfaceGeneration generation,
			WindowsViewportSurfaceState& inOutState) override;
		Failure BeginFrame(WindowsViewportSurfaceState& state) override;
		Failure ExportFrame(
			WindowsViewportSurfaceState& state,
			FramePacket& outFrame) override;
		Failure ReleaseSurface(const WindowsViewportSurfaceState& state) override;
		Failure GetLastFailure() const override;

		std::string BuildSummary(
			ViewportId viewportId,
			ConnectionEpoch epoch,
			SurfaceGeneration generation) const;

	private:
		struct Impl;
		TUniquePtr<Impl> m_impl;
	};

	class SailorWindowsViewportPresenter final : public IWindowsViewportPresenter
	{
	public:
		SailorWindowsViewportPresenter();
		~SailorWindowsViewportPresenter() override;

		Failure ImportSurface(
			const ViewportDescriptor& viewport,
			const TransportDescriptor& transport,
			ConnectionEpoch epoch,
			SurfaceGeneration generation) override;
		Failure PresentFrame(ViewportId viewportId, const FramePacket& frame) override;
		void ResetViewport(ViewportId viewportId) override;
		Failure GetLastFailure() const override;

		Failure BindNativeHost(void* swapChainPanelInspectable, float compositionScale);
		std::string BuildSummary(ViewportId viewportId) const;

	private:
		struct Impl;
		TUniquePtr<Impl> m_impl;
	};
}

#endif
