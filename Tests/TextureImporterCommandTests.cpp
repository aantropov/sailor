#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "RHI/Texture.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace Sailor;

namespace Sailor
{
	class TextureImporterTestAccess
	{
	public:
		using Decoder = bool (*)(const TextureImporter::CpuDecodeRequest&, TextureImporter::ByteCode&,
			int32_t&, int32_t&, uint32_t&);
		static Decoder ExchangeDecoder(TextureImporter& importer, Decoder decoder)
		{
			return std::exchange(importer.m_decodeTexture, decoder);
		}
	};
}

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	class TextureFixture final
	{
	public:
		TextureFixture(const std::filesystem::path& workspace, const char* name) :
			m_path(workspace / "Content" / (std::string(name) + ".tga"))
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
			Require(registry->GetOrLoadFile(m_path.string()) == m_id, "texture fixture must register");
			m_info = registry->GetAssetInfoPtr<TextureAssetInfoPtr>(m_id);
		}

		void Write(uint8_t red, uint8_t blue)
		{
			const bool existed = std::filesystem::exists(m_path);
			const auto previousTime = existed ? std::filesystem::last_write_time(m_path) :
				std::filesystem::file_time_type{};
			std::array<uint8_t, 21> bytes{};
			bytes[2] = 2;
			bytes[12] = bytes[14] = 1;
			bytes[16] = 24;
			bytes[18] = blue;
			bytes[20] = red;
			std::ofstream output(m_path, std::ios::binary);
			output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			output.close();
			Require(static_cast<bool>(output), "texture fixture must be written");
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

		std::filesystem::path m_path;
		FileId m_id;
		TextureAssetInfoPtr m_info = nullptr;
	};

	class DecodeProbe final
	{
	public:
		DecodeProbe(FileId id, bool holdFirst = false) : m_id(id), m_holdFirst(holdFirst)
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
			if (!m_released.exchange(true)) m_release.count_down();
		}
		void WaitDecoded(uint32_t count)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (m_decoded.load() < count && std::chrono::steady_clock::now() < deadline)
			{
				std::this_thread::yield();
			}
			Require(m_decoded.load() >= count, "the real texture decoder must reach the controlled publication boundary");
		}
		void CheckWorker(uint32_t count) const
		{
			Require(m_decoded.load() == count && !m_wrongThread.load(),
				"all cold, enrichment and reload decoding must execute on Worker");
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
					probe->m_wrongThread = true;
				}
				const auto index = probe->m_decoded.fetch_add(1);
				if (index == 0 && probe->m_holdFirst) probe->m_release.wait();
			}
			return result;
		}
		static inline DecodeProbe* s_active = nullptr;
		FileId m_id;
		bool m_holdFirst;
		TextureImporterTestAccess::Decoder m_original;
		std::atomic<uint32_t> m_decoded{ 0 };
		std::atomic<bool> m_wrongThread{ false }, m_released{ false };
		std::latch m_release{ 1 };
	};

	void TestCpuEnrichment(const std::filesystem::path& workspace, bool collectPromises)
	{
		TextureFixture fixture(workspace, collectPromises ? "CpuAfterGc" : "CpuBeforeGc");
		DecodeProbe decode(fixture.m_id);
		auto* importer = App::GetSubmodule<TextureImporter>();
		TexturePtr texture;
		Require(importer->LoadTexture_Immediate(fixture.m_id, texture) && texture && texture->GetRHI(),
			"fixture must load a real GPU texture");
		Require(!texture->HasCpuData(), "cold load must honor disabled CPU retention");
		const auto rhi = texture->GetRHI();
		const auto slot = importer->GetTextureIndex(fixture.m_id);
		const auto slots = importer->GetTextureSamplersCount();
		const auto bindings = importer->GetTextureSamplersSnapshot({ static_cast<uint32_t>(slot) });
		if (collectPromises) importer->CollectGarbage();

		fixture.KeepCpu(true);
		TexturePtr upgraded;
		auto load = importer->LoadTexture(fixture.m_id, upgraded);
		Require(load.IsValid(), "CPU enrichment must produce a task");
		load->Wait();
		const bool identity = upgraded == texture && load->GetResult() == texture &&
			importer->GetLoadedTexture(fixture.m_id) == texture && texture->GetRHI() == rhi &&
			importer->GetTextureIndex(fixture.m_id) == slot && importer->GetTextureSamplersCount() == slots;
		Require(identity, "CPU enrichment must retain Texture, GPU image and sampler slot identity");
		Require(texture->HasCpuData() && texture->GetDecodedData().Num() == 4 &&
			texture->GetDecodedData()[0] == 255 && texture->GetDecodedData()[2] == 0,
			"CPU enrichment must complete even while the finished cold-load promise is cached");
		const auto enriched = importer->GetTextureSamplersSnapshot({ static_cast<uint32_t>(slot) });
		Require(enriched.m_descriptorRevision == bindings.m_descriptorRevision &&
			enriched.m_slots[0].m_contentRevision == bindings.m_slots[0].m_contentRevision,
			"CPU enrichment must not update descriptors");
		importer->CollectGarbage();
		TexturePtr repeated;
		Require(importer->LoadTexture_Immediate(fixture.m_id, repeated) && repeated == texture &&
			importer->GetTextureSamplersCount() == slots && repeated->GetRHI() == rhi,
			"repeated CPU requests must reuse the enriched texture");
		decode.CheckWorker(2);
	}

	void TestPendingCpuRequest(const std::filesystem::path& workspace)
	{
		TextureFixture fixture(workspace, "CpuDuringLoad");
		auto* importer = App::GetSubmodule<TextureImporter>();
		DecodeProbe decode(fixture.m_id, true);
		const auto slots = importer->GetTextureSamplersCount();
		TexturePtr texture;
		auto cold = importer->LoadTexture(fixture.m_id, texture);
		decode.WaitDecoded(1);
		fixture.KeepCpu(true);
		TexturePtr enriched, repeated;
		auto upgrade = importer->LoadTexture(fixture.m_id, enriched);
		auto duplicate = importer->LoadTexture(fixture.m_id, repeated);
		Require(upgrade != cold && duplicate == upgrade && enriched == texture && repeated == texture,
			"a late CPU request must chain after cold load and coalesce subsequent requests");
		importer->CollectGarbage();
		Require(importer->GetLoadPromise(fixture.m_id) == upgrade && !texture->HasCpuData(),
			"GC must retain the pending CPU publication; readers must not see a partial buffer");
		decode.Release();
		upgrade->Wait();
		Require(upgrade->GetResult() == texture && texture->HasCpuData() &&
			texture->GetDecodedData()[0] == 255 && importer->GetTextureSamplersCount() == slots + 1,
			"pending cold load plus CPU request must publish one texture and one slot");
		decode.CheckWorker(2);
	}

	void TestReloadOrdering(const std::filesystem::path& workspace, bool missingMiddle)
	{
		TextureFixture fixture(workspace, missingMiddle ? "MissingMiddle" : "ReloadOrder");
		fixture.KeepCpu(true);
		auto* importer = App::GetSubmodule<TextureImporter>();
		TexturePtr texture;
		Require(importer->LoadTexture_Immediate(fixture.m_id, texture), "ordering fixture must load");
		const auto slot = importer->GetTextureIndex(fixture.m_id);
		const auto slots = importer->GetTextureSamplersCount();
		DecodeProbe decode(fixture.m_id, true);
		importer->OnUpdateAssetInfo(fixture.m_info, true);
		auto first = importer->GetLoadPromise(fixture.m_id);
		decode.WaitDecoded(1);
		Tasks::TaskPtr<TexturePtr> missing;
		if (missingMiddle)
		{
			const auto saved = fixture.m_path.string() + ".saved";
			std::filesystem::rename(fixture.m_path, saved);
			importer->OnUpdateAssetInfo(fixture.m_info, true);
			missing = importer->GetLoadPromise(fixture.m_id);
			std::filesystem::rename(saved, fixture.m_path);
		}
		fixture.Clamp(RHI::ETextureClamping::Clamp);
		importer->OnUpdateAssetInfo(fixture.m_info, true);
		auto latest = importer->GetLoadPromise(fixture.m_id);
		const uint32_t requests = missingMiddle ? 3 : 2;
		if (App::GetSubmodule<Tasks::Scheduler>()->GetNumThreads(EThreadType::Worker) > 1)
		{
			decode.WaitDecoded(requests);
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
		}
		const bool stayedPending = latest != first && !latest->IsFinished();
		decode.Release();
		latest->Wait();
		first->Wait();
		Require(stayedPending && latest->GetResult() == texture &&
			texture->GetRHI()->GetClamping() == RHI::ETextureClamping::Clamp &&
			texture->HasCpuData() && importer->GetTextureIndex(fixture.m_id) == slot &&
			importer->GetTextureSamplersCount() == slots,
			"reload publication must follow request order even when newer decoding finishes first");
		if (missing) Require(!missing->GetResult(), "a missing source must fail without breaking the publication chain");
		decode.CheckWorker(requests);
	}

	void TestEnrichmentAndReload(const std::filesystem::path& workspace)
	{
		TextureFixture fixture(workspace, "CpuAndReload");
		auto* importer = App::GetSubmodule<TextureImporter>();
		TexturePtr texture;
		Require(importer->LoadTexture_Immediate(fixture.m_id, texture), "overlap fixture must load");
		const auto slot = importer->GetTextureIndex(fixture.m_id);
		const auto slots = importer->GetTextureSamplersCount();
		fixture.KeepCpu(true);
		DecodeProbe decode(fixture.m_id, true);
		TexturePtr enriched;
		auto old = importer->LoadTexture(fixture.m_id, enriched);
		decode.WaitDecoded(1);
		fixture.Write(0, 255);
		importer->OnUpdateAssetInfo(fixture.m_info, true);
		auto latest = importer->GetLoadPromise(fixture.m_id);
		decode.Release();
		latest->Wait();
		Require(!old->GetResult() && latest->GetResult() == texture && enriched == texture &&
			texture->HasCpuData() && texture->GetDecodedData()[0] == 0 && texture->GetDecodedData()[2] == 255 &&
			importer->GetTextureIndex(fixture.m_id) == slot && importer->GetTextureSamplersCount() == slots,
			"an obsolete CPU decode must not overwrite a newer hot reload");
		decode.CheckWorker(2);
	}

	void TestColdFailureRetry(const std::filesystem::path& workspace)
	{
		TextureFixture fixture(workspace, "ColdFailure");
		auto* importer = App::GetSubmodule<TextureImporter>();
		{
			std::ofstream invalid(fixture.m_path, std::ios::binary);
			invalid << "not an image";
		}
		const auto slots = importer->GetTextureSamplersCount();
		DecodeProbe decode(fixture.m_id);
		TexturePtr texture;
		Require(!importer->LoadTexture_Immediate(fixture.m_id, texture) && texture &&
			!texture->GetRHI() && !texture->HasCpuData() && importer->GetTextureSamplersCount() == slots,
			"failed cold decode must not report success or reserve a texture slot");
		const auto failedTexture = texture;
		fixture.Write(255, 0);
		Require(importer->LoadTexture_Immediate(fixture.m_id, texture) && texture == failedTexture &&
			texture->GetRHI() && importer->GetTextureSamplersCount() == slots + 1,
			"cold loading must retry a completed failure without changing object identity");
		decode.CheckWorker(2);
	}

	void TestFirstCpuPublication(const std::filesystem::path& workspace)
	{
		TextureFixture fixture(workspace, "CpuPublication");
		auto* importer = App::GetSubmodule<TextureImporter>();
		TexturePtr texture;
		Require(importer->LoadTexture_Immediate(fixture.m_id, texture), "CPU publication fixture must load");
		fixture.KeepCpu(true);
		DecodeProbe decode(fixture.m_id, true);
		TexturePtr enriched;
		auto task = importer->LoadTexture(fixture.m_id, enriched);
		decode.WaitDecoded(1);
		std::atomic<bool> completePixels{ false };
		std::jthread read([&](std::stop_token stop)
			{
				while (!texture->HasCpuData() && !stop.stop_requested()) std::this_thread::yield();
				if (texture->HasCpuData())
				{
					const auto& pixels = texture->GetDecodedData();
					completePixels = pixels.Num() == 4 && pixels[0] == 255 && pixels[1] == 0 &&
						pixels[2] == 0 && pixels[3] == 255;
				}
			});
		decode.Release();
		task->Wait();
		read.request_stop();
		read.join();
		Require(task->GetResult() == texture && completePixels.load(),
			"first CPU readiness must publish the complete pixel vector to an existing reader");
		decode.CheckWorker(1);
	}

	void TestSourceMismatchAndFailure(const std::filesystem::path& workspace)
	{
		TextureFixture fixture(workspace, "SourceMismatch");
		auto* importer = App::GetSubmodule<TextureImporter>();
		TexturePtr texture;
		Require(importer->LoadTexture_Immediate(fixture.m_id, texture), "mismatch fixture must load");
		const auto originalRhi = texture->GetRHI();
		const auto slot = static_cast<uint32_t>(importer->GetTextureIndex(fixture.m_id));
		const auto slots = importer->GetTextureSamplersCount();
		const auto descriptor = importer->GetTextureSamplersSnapshot({ slot }).m_descriptorRevision;
		fixture.Write(0, 255);
		fixture.KeepCpu(true);
		DecodeProbe decode(fixture.m_id);
		TexturePtr enriched;
		auto mismatch = importer->LoadTexture(fixture.m_id, enriched);
		mismatch->Wait();
		Require(!mismatch->GetResult() && enriched == texture && !texture->HasCpuData() &&
			texture->GetRHI() == originalRhi && importer->GetTextureSamplersSnapshot({ slot }).m_descriptorRevision == descriptor,
			"CPU enrichment must not combine new source pixels with an older GPU image");
		importer->OnUpdateAssetInfo(fixture.m_info, true);
		auto updated = importer->GetLoadPromise(fixture.m_id);
		updated->Wait();
		Require(updated->GetResult() == texture && texture->HasCpuData() && texture->GetDecodedData()[2] == 255,
			"explicit reload must recover the mismatched CPU source");
		const auto goodRhi = texture->GetRHI();
		const auto goodRevision = importer->GetTextureSamplersSnapshot({ slot }).m_descriptorRevision;
		{
			std::ofstream invalid(fixture.m_path, std::ios::binary);
			invalid << "not an image";
		}
		importer->OnUpdateAssetInfo(fixture.m_info, true);
		auto failed = importer->GetLoadPromise(fixture.m_id);
		failed->Wait();
		Require(!failed->GetResult() && texture->GetRHI() == goodRhi && texture->GetDecodedData()[2] == 255 &&
			importer->GetTextureSamplersSnapshot({ slot }).m_descriptorRevision == goodRevision,
			"failed reload must preserve the last good GPU texture, descriptors and CPU pixels");
		importer->CollectGarbage();
		fixture.Write(255, 0);
		importer->OnUpdateAssetInfo(fixture.m_info, true);
		auto retry = importer->GetLoadPromise(fixture.m_id);
		retry->Wait();
		Require(retry->GetResult() == texture && texture->GetDecodedData()[0] == 255 &&
			importer->GetTextureIndex(fixture.m_id) == slot && importer->GetTextureSamplersCount() == slots,
			"reload must retry successfully after failure and promise collection");
		decode.CheckWorker(4);
	}

	void TestReloadGcStress(const std::filesystem::path& workspace)
	{
		TextureFixture fixture(workspace, "ReloadGcStress");
		auto* importer = App::GetSubmodule<TextureImporter>();
		TexturePtr texture;
		Require(importer->LoadTexture_Immediate(fixture.m_id, texture), "GC fixture must load");
		const auto slots = importer->GetTextureSamplersCount();
		DecodeProbe decode(fixture.m_id, true);
		std::jthread collect([&](std::stop_token stop)
			{
				while (!stop.stop_requested())
				{
					importer->CollectGarbage();
					std::this_thread::yield();
				}
			});
		for (uint32_t i = 0; i < 24; ++i)
		{
			fixture.Clamp(i % 2 ? RHI::ETextureClamping::Clamp : RHI::ETextureClamping::Repeat);
			importer->OnUpdateAssetInfo(fixture.m_info, true);
		}
		auto last = importer->GetLoadPromise(fixture.m_id);
		Require(last.IsValid() && !last->IsFinished(), "GC must retain the latest pending reload");
		collect.request_stop();
		collect.join();
		decode.Release();
		last->Wait();
		Require(last->GetResult() == texture && texture->GetRHI()->GetClamping() == RHI::ETextureClamping::Clamp &&
			importer->GetTextureSamplersCount() == slots, "concurrent promise GC must preserve reload order and slot ownership");
		decode.CheckWorker(24);
	}
}

namespace Sailor::Tests
{
	void RunTextureImporterCommandTests(const std::filesystem::path& workspace)
	{
		std::string failures;
		auto run = [&](const char* name, auto test)
		{
			try { test(); }
			catch (const std::exception& error) { failures += std::string(name) + ": " + error.what() + '\n'; }
		};
		for (bool collectPromises : { false, true })
		{
			try { TestCpuEnrichment(workspace, collectPromises); }
			catch (const std::exception& error)
			{
				failures += std::string(collectPromises ? "CPU after GC: " : "CPU before GC: ") + error.what() + '\n';
			}
		}
		run("Pending CPU request", [&]() { TestPendingCpuRequest(workspace); });
		run("Reload ordering", [&]() { TestReloadOrdering(workspace, false); });
		run("Missing source ordering", [&]() { TestReloadOrdering(workspace, true); });
		run("Enrichment and reload", [&]() { TestEnrichmentAndReload(workspace); });
		run("Cold failure retry", [&]() { TestColdFailureRetry(workspace); });
		run("First CPU publication", [&]() { TestFirstCpuPublication(workspace); });
		run("Source mismatch and failure", [&]() { TestSourceMismatchAndFailure(workspace); });
		run("Reload GC stress", [&]() { TestReloadGcStress(workspace); });
		if (!failures.empty()) throw std::runtime_error(failures);
		std::cout << "Texture importer CPU enrichment, Worker decode, ordered reload and failure tests passed\n";
	}
}
