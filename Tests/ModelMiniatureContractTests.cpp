#include "AssetRegistry/Model/ModelMiniature.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Raytracing/PathTracer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <yaml-cpp/yaml.h>

using namespace Sailor;

namespace
{
	constexpr std::array<uint8_t, 8> PngSignature =
	{
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
	};

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	class TempDirectory final
	{
	public:
		explicit TempDirectory(const std::string& label)
		{
			std::random_device random;
			for (uint32_t attempt = 0; attempt < 100; ++attempt)
			{
				m_path =
					std::filesystem::temp_directory_path() /
					(
						"sailor-" +
						label +
						"-" +
						std::to_string(random()) +
						"-" +
						std::to_string(attempt));
				std::error_code createError;
				if (std::filesystem::create_directory(m_path, createError))
				{
					return;
				}
			}

			throw std::runtime_error(
				"cannot create a unique temporary directory for " + label);
		}

		~TempDirectory()
		{
			std::error_code cleanupError;
			std::filesystem::remove_all(m_path, cleanupError);
		}

		std::filesystem::path Path(const std::filesystem::path& relative) const
		{
			return m_path / relative;
		}

	private:
		std::filesystem::path m_path;
	};

	FileId MakeFileId(const std::string& value)
	{
		FileId result;
		result.Deserialize(YAML::Node(value));
		return result;
	}

	uint32_t ReadBigEndianUint32(const TVector<uint8_t>& bytes, size_t offset)
	{
		Require(bytes.Num() >= offset + sizeof(uint32_t), "PNG field must fit inside the encoded payload");
		return
			(static_cast<uint32_t>(bytes[offset + 0]) << 24u) |
			(static_cast<uint32_t>(bytes[offset + 1]) << 16u) |
			(static_cast<uint32_t>(bytes[offset + 2]) << 8u) |
			static_cast<uint32_t>(bytes[offset + 3]);
	}

	void RequireOutputWasCleared(const TVector<uint8_t>& output, const std::string& context)
	{
		Require(output.Num() == 0, context + " must clear stale caller output");
	}

	void TestCachePathUsesModelsFolderAndUidFilename()
	{
		const std::filesystem::path cacheRoot =
			std::filesystem::path("CacheRoot") / "NestedCache";
		const std::string bracedUid =
			"{14A75BF9-09AC-49EE-A0DF-2945F4B452A6}";
		const std::string unbracedUid =
			"14A75BF9-09AC-49EE-A0DF-2945F4B452A6";

		Require(
			ModelMiniature::GetCachePath(
				cacheRoot,
				MakeFileId(bracedUid)) ==
					cacheRoot / "Models" / (bracedUid + ".png"),
			"braced model FileId path must be exactly <cache>/Models/<UID>.png");
		Require(
			ModelMiniature::GetCachePath(
				cacheRoot,
				MakeFileId(unbracedUid)) ==
					cacheRoot / "Models" / (unbracedUid + ".png"),
			"unbraced model FileId path must be exactly <cache>/Models/<UID>.png");
	}

	void TestCachePathRejectsUnsafeOrNonGuidFileIds()
	{
		const std::filesystem::path cacheRoot = "CacheRoot";
		const std::string invalidFileIds[] = {
			"",
			"NullFileId",
			"../14A75BF9-09AC-49EE-A0DF-2945F4B452A6",
			"/14A75BF9-09AC-49EE-A0DF-2945F4B452A6",
			"{14A75BF9-09AC-49EE-A0DF-2945F4B452A6}/preview"
		};
		for (const std::string& invalidFileId : invalidFileIds)
		{
			Require(
				ModelMiniature::GetCachePath(
					cacheRoot,
					MakeFileId(invalidFileId)).empty(),
				"unsafe or non-GUID FileId must not resolve a miniature path");
		}
	}

	void TestMiniatureConstants()
	{
		Require(ModelMiniature::Resolution == 256u, "model miniature resolution must remain 256");
		Require(
			ModelMiniature::TextureResolution == 512u,
			"model miniature source textures must remain capped at 512");
		Require(
			std::string(ModelMiniature::CacheFolder) == "Models",
			"model miniature cache folder must remain Models");
		Require(
			std::string(ModelMiniature::Extension) == ".png",
			"model miniature extension must remain .png");
		Require(
			std::string(ModelMiniature::FingerprintExtension) == ".revision",
			"model miniature fingerprint extension must remain .revision");
	}

	FileRevision MakeRevision(
		int64_t modificationTime,
		uint64_t fileSize,
		uint64_t contentHash)
	{
		return FileRevision{
			modificationTime,
			fileSize,
			contentHash,
			true
		};
	}

	void TestMiniatureFingerprintTracksSourceMetadataAndResolution()
	{
		TempDirectory temporaryDirectory("model-miniature-fingerprint");
		const FileId fileId = MakeFileId(
			"{14A75BF9-09AC-49EE-A0DF-2945F4B452A6}");
		const FileRevision sourceRevision = MakeRevision(10, 20, 30);
		const FileRevision metadataRevision = MakeRevision(40, 50, 60);
		const std::filesystem::path miniaturePath =
			ModelMiniature::GetCachePath(
				temporaryDirectory.Path("Cache"),
				fileId);
		std::error_code createError;
		std::filesystem::create_directories(
			miniaturePath.parent_path(),
			createError);
		Require(!createError, "miniature cache directory must be creatable");
		TVector<u8vec4> pixels(
			static_cast<size_t>(ModelMiniature::Resolution) *
			ModelMiniature::Resolution);
		for (u8vec4& pixel : pixels)
		{
			pixel = u8vec4(64u, 128u, 192u, 255u);
		}
		TVector<uint8_t> png;
		Require(
			Raytracing::PathTracer::EncodePng(
				pixels,
				glm::uvec2(
					ModelMiniature::Resolution,
					ModelMiniature::Resolution),
				png),
			"miniature fingerprint fixture must encode a valid PNG");
		std::ofstream miniature(miniaturePath, std::ios::binary);
		miniature.write(
			reinterpret_cast<const char*>(png.GetData()),
			static_cast<std::streamsize>(png.Num()));
		miniature.close();
		Require(
			static_cast<bool>(miniature),
			"miniature fixture must be writable");

		std::string diagnostic;
		Require(
			ModelMiniature::SaveFingerprint(
				temporaryDirectory.Path("Cache"),
				fileId,
				sourceRevision,
				metadataRevision,
				diagnostic),
			"valid miniature fingerprint must be saved: " + diagnostic);
		Require(
			ModelMiniature::IsCurrent(
				temporaryDirectory.Path("Cache"),
				fileId,
				sourceRevision,
				metadataRevision),
			"matching source and metadata revisions must keep the miniature current");
		Require(
			!ModelMiniature::IsCurrent(
				temporaryDirectory.Path("Cache"),
				fileId,
				MakeRevision(10, 20, 31),
				metadataRevision),
			"a source content change must invalidate the miniature");
		Require(
			!ModelMiniature::IsCurrent(
				temporaryDirectory.Path("Cache"),
				fileId,
				sourceRevision,
				MakeRevision(40, 50, 61)),
			"a metadata content change must invalidate the miniature");

		std::ofstream changedMiniature(
			miniaturePath,
			std::ios::binary | std::ios::app);
		changedMiniature.put('\0');
		changedMiniature.close();
		Require(
			!ModelMiniature::IsCurrent(
				temporaryDirectory.Path("Cache"),
				fileId,
				sourceRevision,
				metadataRevision),
			"a changed PNG must invalidate its fingerprint");
		Require(
			ModelMiniature::SaveFingerprint(
				temporaryDirectory.Path("Cache"),
				fileId,
				sourceRevision,
				metadataRevision,
				diagnostic),
			"the changed PNG fixture must receive a refreshed fingerprint");
		Require(
			ModelMiniature::IsCurrent(
				temporaryDirectory.Path("Cache"),
				fileId,
				sourceRevision,
				metadataRevision),
			"a refreshed PNG fingerprint must become current");

		std::ofstream corruptFingerprint(
			ModelMiniature::GetFingerprintPath(
				temporaryDirectory.Path("Cache"),
				fileId));
		corruptFingerprint
			<< "1 " << (ModelMiniature::Resolution + 1) << '\n';
		corruptFingerprint.close();
		Require(
			!ModelMiniature::IsCurrent(
				temporaryDirectory.Path("Cache"),
				fileId,
				sourceRevision,
				metadataRevision),
			"a fingerprint for another render resolution must be stale");
	}

	void TestEncodePngProduces256SquareRgbaImage()
	{
		constexpr uint32_t Resolution = ModelMiniature::Resolution;
		TVector<u8vec4> image(static_cast<size_t>(Resolution) * Resolution);
		for (uint32_t y = 0; y < Resolution; ++y)
		{
			for (uint32_t x = 0; x < Resolution; ++x)
			{
				image[x + y * Resolution] = u8vec4(
					static_cast<uint8_t>(x),
					static_cast<uint8_t>(y),
					static_cast<uint8_t>(x + y),
					255u);
			}
		}

		TVector<uint8_t> png;
		Require(
			Raytracing::PathTracer::EncodePng(
				image,
				glm::uvec2(Resolution, Resolution),
				png),
			"256x256 RGBA pixels must encode as PNG");
		Require(png.Num() >= 33u, "encoded PNG must contain its signature and IHDR chunk");

		for (size_t index = 0; index < PngSignature.size(); ++index)
		{
			Require(png[index] == PngSignature[index], "encoded payload must begin with the PNG signature");
		}

		Require(ReadBigEndianUint32(png, 8u) == 13u, "PNG IHDR data length must be 13 bytes");
		Require(
			png[12] == 'I' && png[13] == 'H' && png[14] == 'D' && png[15] == 'R',
			"PNG first chunk must be IHDR");
		Require(ReadBigEndianUint32(png, 16u) == Resolution, "PNG IHDR width must be 256");
		Require(ReadBigEndianUint32(png, 20u) == Resolution, "PNG IHDR height must be 256");
		Require(png[24] == 8u, "PNG miniature must use 8-bit channels");
		Require(png[25] == 6u, "PNG miniature must use RGBA color type");
	}

	void TestEncodePngRejectsInvalidExtent()
	{
		TVector<u8vec4> image(1);
		image[0] = u8vec4(255u);

		TVector<uint8_t> png;
		png.Add(0xabu);
		Require(
			!Raytracing::PathTracer::EncodePng(image, glm::uvec2(0u, 1u), png),
			"zero-width images must be rejected");
		RequireOutputWasCleared(png, "zero-width rejection");

		png.Add(0xabu);
		Require(
			!Raytracing::PathTracer::EncodePng(image, glm::uvec2(1u, 0u), png),
			"zero-height images must be rejected");
		RequireOutputWasCleared(png, "zero-height rejection");
	}

	void TestEncodePngRejectsInsufficientPixelData()
	{
		constexpr uint32_t Resolution = ModelMiniature::Resolution;
		const size_t requiredPixels = static_cast<size_t>(Resolution) * Resolution;
		TVector<u8vec4> image(requiredPixels - 1u);

		TVector<uint8_t> png;
		png.Add(0xabu);
		Require(
			!Raytracing::PathTracer::EncodePng(
				image,
				glm::uvec2(Resolution, Resolution),
				png),
			"pixel data smaller than width times height must be rejected");
		RequireOutputWasCleared(png, "insufficient-data rejection");
	}

	void TestExtractTextureFromExternalGltfImage()
	{
		TempDirectory temporaryDirectory(
			"model-miniature-external-gltf-texture");

		TVector<u8vec4> pixels(4);
		pixels[0] = u8vec4(255u, 0u, 0u, 255u);
		pixels[1] = u8vec4(0u, 255u, 0u, 255u);
		pixels[2] = u8vec4(0u, 0u, 255u, 255u);
		pixels[3] = u8vec4(255u, 255u, 255u, 255u);
		TVector<uint8_t> expectedPng;
		Require(
			Raytracing::PathTracer::EncodePng(
				pixels,
				glm::uvec2(2u, 2u),
				expectedPng),
			"external glTF texture fixture must encode");

		const std::filesystem::path texturePath =
			temporaryDirectory.Path("texture.png");
		std::ofstream textureFile(texturePath, std::ios::binary);
		textureFile.write(
			reinterpret_cast<const char*>(expectedPng.GetData()),
			static_cast<std::streamsize>(expectedPng.Num()));
		textureFile.close();
		Require(
			static_cast<bool>(textureFile),
			"external glTF texture fixture must be writable");

		const std::filesystem::path modelPath =
			temporaryDirectory.Path("textured.gltf");
		std::ofstream modelFile(modelPath);
		modelFile
			<< "{\n"
			<< "  \"asset\": { \"version\": \"2.0\" },\n"
			<< "  \"images\": [ { \"uri\": \"texture.png\" } ],\n"
			<< "  \"textures\": [ { \"source\": 0 } ]\n"
			<< "}\n";
		modelFile.close();
		Require(
			static_cast<bool>(modelFile),
			"external glTF model fixture must be writable");

		TextureImporter::ByteCode actualPng;
		Require(
			TextureImporter::ExtractTextureFromModelSource(
				modelPath.string(),
				0,
				actualPng),
			"TextureImporter must extract external glTF image bytes");
		Require(
			actualPng.Num() == expectedPng.Num() &&
			std::equal(
				actualPng.begin(),
				actualPng.end(),
				expectedPng.begin()),
			"extracted external glTF image bytes must match the source PNG");

		TextureImporter::ByteCode invalidTexture;
		invalidTexture.Add(0xabu);
		Require(
			!TextureImporter::ExtractTextureFromModelSource(
				modelPath.string(),
				1,
				invalidTexture),
			"out-of-range glTF texture indices must be rejected");
		Require(
			invalidTexture.Num() == 0,
			"failed glTF texture extraction must clear stale output");

	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "CachePathUsesModelsFolderAndUidFilename", TestCachePathUsesModelsFolderAndUidFilename },
		{ "CachePathRejectsUnsafeOrNonGuidFileIds", TestCachePathRejectsUnsafeOrNonGuidFileIds },
		{ "MiniatureConstants", TestMiniatureConstants },
		{ "MiniatureFingerprintTracksSourceMetadataAndResolution", TestMiniatureFingerprintTracksSourceMetadataAndResolution },
		{ "EncodePngProduces256SquareRgbaImage", TestEncodePngProduces256SquareRgbaImage },
		{ "EncodePngRejectsInvalidExtent", TestEncodePngRejectsInvalidExtent },
		{ "EncodePngRejectsInsufficientPixelData", TestEncodePngRejectsInsufficientPixelData },
		{ "ExtractTextureFromExternalGltfImage", TestExtractTextureFromExternalGltfImage },
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
			std::cerr << "[FAIL] " << test.first << ": " << error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
