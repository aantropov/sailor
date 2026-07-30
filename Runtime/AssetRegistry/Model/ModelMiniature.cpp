#include "AssetRegistry/Model/ModelMiniature.h"

#include "Core/Utils.h"
#include "Workspace/WorkspaceCacheContract.h"

#include <fstream>
#include <sstream>
#include <string>

using namespace Sailor;

namespace
{
	constexpr uint32_t FingerprintVersion = 1;

	std::string SerializeRevision(
		const FileRevision& sourceRevision,
		const FileRevision& metadataRevision,
		const FileRevision& miniatureRevision)
	{
		std::ostringstream payload;
		payload
			<< FingerprintVersion << ' '
			<< ModelMiniature::Resolution << '\n'
			<< sourceRevision.m_modificationTimeNanoseconds << ' '
			<< sourceRevision.m_fileSize << ' '
			<< sourceRevision.m_contentHash << ' '
			<< sourceRevision.m_bIsValid << '\n'
			<< metadataRevision.m_modificationTimeNanoseconds << ' '
			<< metadataRevision.m_fileSize << ' '
			<< metadataRevision.m_contentHash << ' '
			<< metadataRevision.m_bIsValid << '\n'
			<< miniatureRevision.m_modificationTimeNanoseconds << ' '
			<< miniatureRevision.m_fileSize << ' '
			<< miniatureRevision.m_contentHash << ' '
			<< miniatureRevision.m_bIsValid << '\n';
		return payload.str();
	}

	bool TryReadRevision(
		const std::filesystem::path& path,
		FileRevision& outSourceRevision,
		FileRevision& outMetadataRevision,
		FileRevision& outMiniatureRevision)
	{
		outSourceRevision = {};
		outMetadataRevision = {};
		outMiniatureRevision = {};

		std::ifstream payload(path);
		uint32_t version = 0;
		uint32_t resolution = 0;
		if (!payload.is_open() ||
			!(payload >> version >> resolution) ||
			version != FingerprintVersion ||
			resolution != ModelMiniature::Resolution ||
			!(payload >>
				outSourceRevision.m_modificationTimeNanoseconds >>
				outSourceRevision.m_fileSize >>
				outSourceRevision.m_contentHash >>
				outSourceRevision.m_bIsValid) ||
			!(payload >>
				outMetadataRevision.m_modificationTimeNanoseconds >>
				outMetadataRevision.m_fileSize >>
				outMetadataRevision.m_contentHash >>
				outMetadataRevision.m_bIsValid) ||
			!(payload >>
				outMiniatureRevision.m_modificationTimeNanoseconds >>
				outMiniatureRevision.m_fileSize >>
				outMiniatureRevision.m_contentHash >>
				outMiniatureRevision.m_bIsValid))
		{
			return false;
		}

		payload >> std::ws;
		return payload.eof() &&
			outSourceRevision.m_bIsValid &&
			outMetadataRevision.m_bIsValid &&
			outMiniatureRevision.m_bIsValid;
	}
}

bool ModelMiniature::IsCurrent(
	const std::filesystem::path& cacheRoot,
	const FileId& fileId,
	const FileRevision& sourceRevision,
	const FileRevision& metadataRevision)
{
	if (!sourceRevision.m_bIsValid || !metadataRevision.m_bIsValid)
	{
		return false;
	}

	const std::filesystem::path miniaturePath =
		GetCachePath(cacheRoot, fileId);
	const std::filesystem::path fingerprintPath =
		GetFingerprintPath(cacheRoot, fileId);
	if (miniaturePath.empty() || fingerprintPath.empty())
	{
		return false;
	}

	FileRevision currentMiniatureRevision;
	if (!Utils::TryGetFileRevision(
			miniaturePath.string(),
			currentMiniatureRevision))
	{
		return false;
	}

	FileRevision savedSourceRevision;
	FileRevision savedMetadataRevision;
	FileRevision savedMiniatureRevision;
	return TryReadRevision(
			fingerprintPath,
			savedSourceRevision,
			savedMetadataRevision,
			savedMiniatureRevision) &&
		savedSourceRevision == sourceRevision &&
		savedMetadataRevision == metadataRevision &&
		savedMiniatureRevision == currentMiniatureRevision;
}

bool ModelMiniature::SaveFingerprint(
	const std::filesystem::path& cacheRoot,
	const FileId& fileId,
	const FileRevision& sourceRevision,
	const FileRevision& metadataRevision,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	const std::filesystem::path miniaturePath =
		GetCachePath(cacheRoot, fileId);
	const std::filesystem::path fingerprintPath =
		GetFingerprintPath(cacheRoot, fileId);
	FileRevision miniatureRevision;
	if (miniaturePath.empty() ||
		fingerprintPath.empty() ||
		!sourceRevision.m_bIsValid ||
		!metadataRevision.m_bIsValid ||
		!Utils::TryGetFileRevision(
			miniaturePath.string(),
			miniatureRevision))
	{
		outDiagnostic =
			"Cannot save an invalid model miniature fingerprint.";
		return false;
	}

	return Workspace::AtomicReplaceWorkspaceCacheText(
		fingerprintPath,
		SerializeRevision(
			sourceRevision,
			metadataRevision,
			miniatureRevision),
		outDiagnostic);
}
