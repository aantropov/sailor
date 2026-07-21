#pragma once
#include <string>
#include <fstream>
#include <filesystem>
#include <map>
#include <vector>
#include "Sailor.h"
#include "Containers/Containers.h"
#include "AssetRegistry/FileId.h"
#include "Core/Submodule.h"
#include "AssetRegistry/AssetInfo.h"
#include "Core/Singleton.hpp"
#include "Engine/Object.h"
#include "AssetRegistry/AssetCache.h"

namespace Sailor
{
	class AssetInfo;
	using AssetInfoPtr = AssetInfo*;

	enum class EAssetType;

	class AssetRegistry final : public TSubmodule<AssetRegistry>
	{
	public:
		struct AssetReadLocation final
		{
			std::filesystem::path m_physicalPath;
			std::string m_virtualPath;
			EAssetMountKind m_mountKind = EAssetMountKind::Engine;
			bool m_bWritable = false;
		};

		SAILOR_API static std::string GetContentFolder();
		SAILOR_API static std::string GetWorkspaceContentFolder();
		SAILOR_API static std::string GetEngineContentFolder();
		SAILOR_API static std::string GetCacheFolder();

		static constexpr const char* MetaFileExtension = "asset";

		SAILOR_API AssetRegistry();
		SAILOR_API virtual ~AssetRegistry() override;

		template<typename TBinaryType, typename TFilepath>
		static bool ReadBinaryFile(const TFilepath& filename, TVector<TBinaryType>& buffer)
		{
			std::ifstream file(filename, std::ios::ate | std::ios::binary);
			file.unsetf(std::ios::skipws);

			if (!file.is_open())
			{
				return false;
			}

			size_t fileSize = (size_t)file.tellg();
			buffer.Clear();

			size_t mod = fileSize % sizeof(TBinaryType);
			size_t size = fileSize / sizeof(TBinaryType) + (mod ? 1 : 0);
			buffer.Resize(size);

			//buffer.resize(fileSize / sizeof(T));

			file.seekg(0, std::ios::beg);
			file.read(reinterpret_cast<char*>(buffer.GetData()), fileSize);

			file.close();
			return true;
		}

		template<typename TTextType, typename TFilepath>
		static bool ReadTextFile(const TFilepath& filename, TTextType& outText)
		{
			std::ifstream file(filename, std::ios::in | std::ios::ate);
			if (!file.is_open())
			{
				return false;
			}

			file.seekg(0, std::ios::beg);

			std::stringstream buffer;
			buffer << file.rdbuf();
			outText = buffer.str();

			file.close();

			return true;
		}

		template<typename TBinaryType, typename TFilepath>
		static void WriteBinaryFile(const TFilepath& filename, const TVector<TBinaryType>& buffer)
		{
			std::ofstream file(filename, std::ofstream::binary);
			file.write(reinterpret_cast<const char*>(buffer.GetData()), buffer.Num() * sizeof(buffer[0]));
			file.close();
		}

		template<typename TStringType, typename TFilepath>
		static void WriteTextFile(const TFilepath& filename, const TStringType& text)
		{
			std::ofstream file(filename);
			file << text;
			file.close();
		}

		SAILOR_API static bool ReadAllTextFile(const std::string& filename, std::string& text);
		SAILOR_API bool ResolveContentFile(
			const std::string& virtualPath,
			AssetReadLocation& outLocation) const;
		SAILOR_API bool ReadContentText(const std::string& virtualPath, std::string& outText) const;
		SAILOR_API bool GetContentFileModificationTime(
			const std::string& virtualPath,
			std::time_t& outTimestamp) const;
		SAILOR_API bool ResolveWorkspaceContentPathForWrite(
			const std::string& virtualPath,
			std::filesystem::path& outPath) const;

		template<typename TBinaryType>
		bool ReadContentBinary(const std::string& virtualPath, TVector<TBinaryType>& outBuffer) const
		{
			AssetReadLocation location;
			return ResolveContentFile(virtualPath, location) &&
				ReadBinaryFile(location.m_physicalPath, outBuffer);
		}

		SAILOR_API void ScanContentFolder();
		SAILOR_API const FileId& GetOrLoadFile(const std::string& filepath);

		template<typename TAssetInfoPtr = AssetInfoPtr>
		TAssetInfoPtr GetAssetInfoPtr(FileId uid) const
		{
			return 	dynamic_cast<TAssetInfoPtr>(GetAssetInfoPtr_Internal(uid));
		}

		template<typename TAssetInfoPtr = AssetInfoPtr>
		TAssetInfoPtr GetAssetInfoPtr(const std::string& assetFilepath) const
		{
			return 	dynamic_cast<TAssetInfoPtr>(GetAssetInfoPtr_Internal(assetFilepath));
		}

		template<class TAssetInfo>
		void GetAllAssetInfos(TVector<FileId>& outAssetInfos) const
		{
			outAssetInfos.Clear();
			for (const auto& assetInfo : m_loadedAssetInfo)
			{
				if (dynamic_cast<TAssetInfo*>(*assetInfo.m_second))
				{
					outAssetInfos.Add(assetInfo.m_first);
				}
			}
		}

		SAILOR_API bool RegisterAssetInfoHandler(const TVector<std::string>& supportedExtensions, class IAssetInfoHandler* pAssetInfoHandler);
		SAILOR_API static std::string GetMetaFilePath(const std::string& assetFilePath);

		SAILOR_API bool IsAssetExpired(const AssetInfoPtr info) const;
		SAILOR_API void CacheAsset(const AssetInfoPtr info);

		template<typename T>
		TObjectPtr<T> LoadAssetFromFile(const FileId& id, bool bImmediate = true)
		{
			TObjectPtr<Object> out;
			if (const auto& info = GetAssetInfoPtr(id))
			{
				out = LoadAsset(info->GetHandler(), id, bImmediate);
			}

			return out.DynamicCast<T>();
		}

	protected:

		SAILOR_API TObjectPtr<Object> LoadAsset(IAssetInfoHandler* assetInfoHandler, const FileId& id, bool bImmediate);

		SAILOR_API const FileId& LoadFile(const std::string& filepath);

		SAILOR_API AssetInfoPtr GetAssetInfoPtr_Internal(FileId uid) const;
		SAILOR_API AssetInfoPtr GetAssetInfoPtr_Internal(const std::string& assetFilepath) const;
		bool RestoreAssetProcessingTimes(AssetInfoPtr info) const;
		bool ResolveDirectLoadPath(
			const std::string& requestedPath,
			AssetReadLocation& outLocation) const;
		IAssetInfoHandler* GetAssetInfoHandler(const std::string& extension) const;
		IAssetInfoHandler* GetAssetInfoHandler(
			const std::string& extension,
			const std::string& assetInfoType,
			bool bPrimary) const;

		TMap<FileId, AssetInfoPtr> m_loadedAssetInfo;
		TMap<std::string, FileId> m_fileIds;
		TMap<std::string, FileId> m_physicalFileIds;
		TMap<std::string, class IAssetInfoHandler*> m_assetInfoHandlers;
		TVector<AssetMountDescriptor> m_contentMounts;
		TMap<std::string, AssetReadLocation> m_contentFileWinners;

		AssetCache m_assetCache;

		friend class IAssetInfoHandler;
	};
}
