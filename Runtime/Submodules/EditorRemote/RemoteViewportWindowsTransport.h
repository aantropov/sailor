#pragma once
#include "Containers/Containers.h"
#include "Memory/UniquePtr.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "RemoteViewportBinding.h"
#include "RemoteViewportRuntime.h"

namespace Sailor::EditorRemote
{
	class IWindowsViewportPresenter;

	struct WindowsViewportSurfaceKey
	{
		ViewportId m_viewportId = 0;
		ConnectionEpoch m_epoch = 0;
		SurfaceGeneration m_generation = 0;

		auto operator<=>(const WindowsViewportSurfaceKey&) const = default;

		size_t GetHash() const noexcept
		{
			size_t seed = 0;
			HashCombine(seed, m_viewportId, m_epoch, m_generation);
			return seed;
		}
	};

	struct WindowsViewportSurfaceState
	{
		WindowsViewportSurfaceKey m_key{};
		ViewportDescriptor m_viewport{};
		TransportDescriptor m_transport{};
		FrameIndex m_lastExportedFrameIndex = 0;
		bool m_frameBegun = false;
	};

	class IWindowsSharedSurfaceProvider
	{
	public:
		virtual ~IWindowsSharedSurfaceProvider() = default;
		virtual Failure CreateOrResizeSurface(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, WindowsViewportSurfaceState& inOutState) = 0;
		virtual Failure BeginFrame(WindowsViewportSurfaceState& state) = 0;
		virtual Failure ExportFrame(WindowsViewportSurfaceState& state, FramePacket& outFrame) = 0;
		virtual Failure ReleaseSurface(const WindowsViewportSurfaceState& state) = 0;
		virtual Failure GetLastFailure() const = 0;
	};

	class WindowsViewportTransportBackend : public IViewportTransportBackend
	{
	public:
		using Provider = IWindowsSharedSurfaceProvider;
		Failure ImportSurface(IWindowsViewportPresenter& presenter, const ViewportDescriptor& viewport,
			const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation);

		explicit WindowsViewportTransportBackend(IWindowsSharedSurfaceProvider& provider) :
			m_provider(provider)
		{
		}

		Failure EnsureSurface(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, TransportDescriptor& outTransport) override
		{
			auto release = ReleaseSurface(viewport.m_viewportId, epoch, generation);
			if (!release.IsOk()) return release;
			const WindowsViewportSurfaceKey key{ viewport.m_viewportId, epoch, generation };
			auto& storedState = m_surfaces[key];
			storedState = TUniquePtr<WindowsViewportSurfaceState>::Make();
			auto& state = *storedState;
			state.m_key = { viewport.m_viewportId, epoch, generation };
			state.m_viewport = viewport;
			state.m_transport.m_transportType = TransportType::WinSharedHandle;
			state.m_transport.m_syncMode = SyncMode::ExplicitFence;
			state.m_transport.m_protocolVersion = 1;
			state.m_transport.m_width = viewport.m_width;
			state.m_transport.m_height = viewport.m_height;
			state.m_transport.m_pixelFormat = viewport.m_pixelFormat;
			state.m_transport.m_usageFlags = viewport.m_usageFlags;
			state.m_transport.m_ready = false;

			auto result = m_provider.CreateOrResizeSurface(viewport, epoch, generation, state);
			if (!result.IsOk())
			{
				ReleaseSurface(viewport.m_viewportId, epoch, generation);
				m_lastFailure = result;
				return result;
			}

			state.m_transport.m_transportType = TransportType::WinSharedHandle;
			state.m_transport.m_ready = true;
			result = state.m_transport.Validate();
			if (result.IsOk() && (state.m_transport.m_width != viewport.m_width ||
				state.m_transport.m_height != viewport.m_height || state.m_transport.m_pixelFormat != viewport.m_pixelFormat))
			{
				result = Failure::FromDomain(ErrorDomain::Protocol, 1, "Transport does not match its viewport");
			}
			if (!result.IsOk())
			{
				ReleaseSurface(viewport.m_viewportId, epoch, generation);
				m_lastFailure = result;
				return result;
			}

			outTransport = state.m_transport;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure BeginFrame(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation) override
		{
			auto* state = FindSurface(viewport.m_viewportId, epoch, generation);
			if (!state)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2, "Missing Windows transport surface for frame begin");
				return m_lastFailure;
			}
			if (state->m_frameBegun)
			{
				m_lastFailure = Failure::Ok();
				return Failure::Ok();
			}

			auto result = m_provider.BeginFrame(*state);
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

			state->m_frameBegun = true;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure PrepareFrame(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, bool& outReady)
		{
			auto result = BeginFrame(viewport, epoch, generation);
			outReady = result.IsOk();
			return result;
		}

		Failure ExportFrame(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, FramePacket& outFrame) override
		{
			auto* state = FindSurface(viewport.m_viewportId, epoch, generation);
			if (!state)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2, "Missing Windows transport surface for frame export");
				return m_lastFailure;
			}
			if (!state->m_frameBegun)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Protocol, 1, "Windows transport export requires BeginFrame first");
				return m_lastFailure;
			}

			auto result = m_provider.ExportFrame(*state, outFrame);
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

			state->m_lastExportedFrameIndex = outFrame.m_frameIndex;
			state->m_frameBegun = false;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure ReleaseSurface(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation) override
		{
			WindowsViewportSurfaceKey key{ viewportId, epoch, generation };
			auto it = m_surfaces.Find(key);
			if (it == m_surfaces.end())
			{
				m_lastFailure = Failure::Ok();
				return Failure::Ok();
			}

			auto result = m_provider.ReleaseSurface(*it.Value());
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

			m_surfaces.Remove(key);
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure GetLastFailure() const override
		{
			return m_lastFailure;
		}

		const WindowsViewportSurfaceState* FindSurface(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation) const
		{
			WindowsViewportSurfaceKey key{ viewportId, epoch, generation };
			auto it = m_surfaces.Find(key);
			return it != m_surfaces.end() ? it.Value().GetRawPtr() : nullptr;
		}

		size_t GetSurfaceCount() const { return m_surfaces.Num(); }

		Failure ReleaseSurfaces(ViewportId viewportId, ConnectionEpoch keepEpoch = 0, SurfaceGeneration keepGeneration = 0)
		{
			Failure failure = Failure::Ok();
			for (const auto& key : m_surfaces.GetKeys())
			{
				if (key.m_viewportId == viewportId && (key.m_epoch != keepEpoch || key.m_generation != keepGeneration))
				{
					auto result = ReleaseSurface(key.m_viewportId, key.m_epoch, key.m_generation);
					if (!result.IsOk()) failure = result;
				}
			}
			m_lastFailure = failure;
			return failure;
		}

	private:
		WindowsViewportSurfaceState* FindSurface(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation)
		{
			WindowsViewportSurfaceKey key{ viewportId, epoch, generation };
			auto it = m_surfaces.Find(key);
			return it != m_surfaces.end() ? it.Value().GetRawPtr() : nullptr;
		}

		IWindowsSharedSurfaceProvider& m_provider;
		TMap<WindowsViewportSurfaceKey, TUniquePtr<WindowsViewportSurfaceState>> m_surfaces{};
		Failure m_lastFailure = Failure::Ok();
	};

	class IWindowsViewportPresenter
	{
	public:
		virtual ~IWindowsViewportPresenter() = default;
		virtual Failure ImportSurface(const ViewportDescriptor& viewport, const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation) = 0;
		virtual Failure PresentFrame(ViewportId viewportId, const FramePacket& frame) = 0;
		virtual void ResetViewport(ViewportId viewportId) = 0;
		virtual Failure GetLastFailure() const = 0;
	};

	inline Failure WindowsViewportTransportBackend::ImportSurface(IWindowsViewportPresenter& presenter,
		const ViewportDescriptor& viewport, const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation)
	{
		return presenter.ImportSurface(viewport, transport, epoch, generation);
	}

	using WindowsViewportLoopbackBinding = TViewportLoopbackBinding<WindowsViewportTransportBackend, IWindowsViewportPresenter>;
}
