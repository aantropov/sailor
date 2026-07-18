#pragma once

#include <exception>
#include <string>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace Sailor::External
{
	template<typename TCallable>
	bool GuardYamlExceptions(TCallable&& callable, std::string& outDiagnostic) noexcept
	{
		outDiagnostic.clear();
		try
		{
			std::forward<TCallable>(callable)();
			return true;
		}
		catch (const YAML::Exception& exception)
		{
			outDiagnostic = exception.what();
		}
		catch (const std::exception& exception)
		{
			outDiagnostic = exception.what();
		}
		catch (...)
		{
			outDiagnostic = "Unknown YAML dependency failure.";
		}

		return false;
	}

	template<typename TCallable, typename TResult>
	bool TryInvokeYaml(TCallable&& callable, TResult& outResult, std::string& outDiagnostic) noexcept
	{
		return GuardYamlExceptions(
			[&callable, &outResult]()
			{
				outResult = callable();
			},
			outDiagnostic);
	}

	inline bool TryLoadYaml(
		const std::string& payload,
		YAML::Node& outDocument,
		std::string& outDiagnostic) noexcept
	{
		return GuardYamlExceptions(
			[&payload, &outDocument]()
			{
				outDocument = YAML::Load(payload);
			},
			outDiagnostic);
	}

	inline bool TryDumpYaml(
		const YAML::Node& document,
		std::string& outPayload,
		std::string& outDiagnostic) noexcept
	{
		return GuardYamlExceptions(
			[&document, &outPayload]()
			{
				outPayload = YAML::Dump(document);
			},
			outDiagnostic);
	}

	template<typename TValue>
	bool TryConvertYaml(
		const YAML::Node& node,
		TValue& outValue,
		std::string& outDiagnostic) noexcept
	{
		return GuardYamlExceptions(
			[&node, &outValue]()
			{
				outValue = node.as<TValue>();
			},
			outDiagnostic);
	}
}
