#pragma once
#include "TextureImporterTestAccess.h"
#include "AssetRegistry/AssetRegistry.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <latch>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace Sailor::Tests
{
	inline void RequireTextureFixture(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	class TextureFixture final
	{
	public:
		TextureFixture(const std::filesystem::path& workspace, std::string_view name) :
			m_path((workspace / "Content" / name).concat(".tga"))
		{
			Write(255, 0);
			TextureAssetInfo metadataSource;
			auto metadata = metadataSource.Serialize();
			m_id = FileId::CreateNewFileId();
			metadata["fileId"] = m_id;
			metadata["filename"] = m_path.filename().string();
			metadata["bShouldKeepCpuBuffers"] = false;
			metadata["bShouldGenerateMips"] = false;
			{
				std::ofstream output(m_path.string() + ".asset");
				output << metadata;
			}
			auto* registry = App::GetSubmodule<AssetRegistry>();
			RequireTextureFixture(registry->GetOrLoadFile(m_path.string()) == m_id, "texture fixture must register");
			m_info = registry->GetAssetInfoPtr<TextureAssetInfoPtr>(m_id);
		}

		void Write(uint8_t red, uint8_t blue, uint16_t width = 1)
		{
			const bool existed = std::filesystem::exists(m_path);
			const auto previousTime = existed ? std::filesystem::last_write_time(m_path) :
				std::filesystem::file_time_type{};
			std::array<uint8_t, 18> header{};
			header[2] = 2;
			header[12] = static_cast<uint8_t>(width);
			header[13] = static_cast<uint8_t>(width >> 8);
			header[14] = 1;
			header[16] = 24;
			const std::array<uint8_t, 3> pixel{ blue, 0, red };
			std::ofstream output(m_path, std::ios::binary);
			output.write(reinterpret_cast<const char*>(header.data()), header.size());
			for (uint16_t i = 0; i < width; ++i)
			{
				output.write(reinterpret_cast<const char*>(pixel.data()), pixel.size());
			}
			output.close();
			RequireTextureFixture(static_cast<bool>(output), "texture fixture must be written");
			if (existed && std::filesystem::last_write_time(m_path) <= previousTime)
			{
				std::filesystem::last_write_time(m_path, previousTime + std::chrono::seconds(1));
			}
		}

		void KeepCpu(bool enabled)
		{
			auto metadata = m_info->Serialize();
			metadata["bShouldKeepCpuBuffers"] = enabled;
			m_info->Deserialize(metadata);
		}

		void Clamp(RHI::ETextureClamping clamping)
		{
			auto metadata = m_info->Serialize();
			metadata["clamping"] = clamping;
			m_info->Deserialize(metadata);
		}

		void SaveMetadata() const
		{
			std::ofstream output(m_path.string() + ".asset");
			output << m_info->Serialize();
			RequireTextureFixture(static_cast<bool>(output), "texture fixture metadata must be written");
		}

		std::filesystem::path m_path;
		FileId m_id;
		TextureAssetInfoPtr m_info = nullptr;
	};

	class DecodeProbe final
	{
	public:
		DecodeProbe(FileId id, bool holdFirst = false) : m_id(id), m_bShouldHoldFirst(holdFirst)
		{
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(
				{ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
			s_active = this;
			m_original = TextureImporterTestAccess::ExchangeDecoder(*App::GetSubmodule<TextureImporter>(), &Decode);
		}
		~DecodeProbe()
		{
			Release();
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(
				{ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
			TextureImporterTestAccess::ExchangeDecoder(*App::GetSubmodule<TextureImporter>(), m_original);
			s_active = nullptr;
		}
		void Release()
		{
			if (!m_bIsReleased.exchange(true)) m_release.count_down();
		}
		void WaitDecoded(uint32_t count)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (m_decoded.load() < count && std::chrono::steady_clock::now() < deadline)
			{
				std::this_thread::yield();
			}
			RequireTextureFixture(m_decoded.load() >= count, "the real texture decoder must reach the controlled publication boundary");
		}
		void CheckWorker(uint32_t count) const
		{
			RequireTextureFixture(m_decoded.load() == count && !m_bIsWrongThread.load(),
				"all cold, enrichment and reload decoding must execute on Worker");
		}
		void WaitOtherDecoded()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (!m_otherDecoded && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
			RequireTextureFixture(m_otherDecoded > 0, "another texture must finish decoding while the first is held");
		}
		void CheckOtherDecoded(uint32_t count) const
		{
			RequireTextureFixture(m_otherDecoded == count, "independent source images must decode only for their own revisions");
		}

	private:
		static bool Decode(const TextureImporter::CpuDecodeRequest& request, TextureImporter::ByteCode& bytes,
			int32_t& width, int32_t& height, uint32_t& mips)
		{
			auto* probe = s_active;
			const bool result = TextureImporter::DecodeTextureCpu(request, bytes, width, height, mips);
			if (request.m_fileId == probe->m_id)
			{
				if (App::GetSubmodule<Tasks::Scheduler>()->GetCurrentThreadType() != EThreadType::Worker)
				{
					probe->m_bIsWrongThread = true;
				}
				const auto index = probe->m_decoded.fetch_add(1);
				if (index == 0 && probe->m_bShouldHoldFirst) probe->m_release.wait();
			}
			else
			{
				++probe->m_otherDecoded;
			}
			return result;
		}
		static inline DecodeProbe* s_active = nullptr;
		FileId m_id;
		bool m_bShouldHoldFirst;
		TextureImporterTestAccess::Decoder m_original;
		std::atomic<uint32_t> m_decoded{ 0 };
		std::atomic<uint32_t> m_otherDecoded{ 0 };
		std::atomic<bool> m_bIsWrongThread{ false }, m_bIsReleased{ false };
		std::latch m_release{ 1 };
	};
}
