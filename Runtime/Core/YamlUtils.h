#pragma once

#include "Core/Defines.h"
#include "Containers/Containers.h"
#include "YamlExceptionBoundary.h"

#include <cstddef>
#include <string>

#include <yaml-cpp/yaml.h>

namespace Sailor::Utils
{
	enum class EYamlMapValidationError
	{
		None,
		ExpectedMap,
		NonScalarKey,
		EmptyKey,
		DuplicateKey,
		UnknownField,
		MissingField
	};

	struct SAILOR_API YamlMapValidationResult final
	{
		EYamlMapValidationError m_error = EYamlMapValidationError::None;
		std::string m_fieldName{};

		bool IsValid() const noexcept
		{
			return m_error == EYamlMapValidationError::None;
		}
	};

	SAILOR_API bool TryLoadSingleYamlDocument(
		const std::string& payload,
		YAML::Node& outDocument,
		std::string& outDiagnostic) noexcept;

	SAILOR_API size_t CountYamlMapField(
		const YAML::Node& map,
		const std::string& fieldName,
		YAML::Node* outField = nullptr);

	SAILOR_API YAML::Node FindYamlMapField(
		const YAML::Node& map,
		const std::string& fieldName);

	SAILOR_API YamlMapValidationResult ValidateYamlMap(
		const YAML::Node& map);

	SAILOR_API YamlMapValidationResult ValidateYamlMapFields(
		const YAML::Node& map,
		const TVector<std::string>& requiredFields,
		const TVector<std::string>& optionalFields = {});

	template<typename TValue>
	bool TryDecodeYamlScalar(
		const YAML::Node& node,
		TValue& outValue,
		std::string& outDiagnostic) noexcept
	{
		if (!node.IsDefined() || !node.IsScalar())
		{
			outDiagnostic = "expected a YAML scalar";
			return false;
		}

		return External::TryConvertYaml(node, outValue, outDiagnostic);
	}

	template<typename TValue>
	bool TryDecodeYamlScalar(
		const YAML::Node& node,
		TValue& outValue) noexcept
	{
		std::string diagnostic;
		return TryDecodeYamlScalar(node, outValue, diagnostic);
	}
}
