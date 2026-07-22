#pragma once

#include "Containers/Vector.h"

#include <string>
#include <utility>

namespace Sailor
{
	class ShaderYamlIncludeResolver final
	{
	public:
		template<typename TReader>
		static bool Append(
			const TVector<std::string>& includes,
			TReader&& reader,
			std::string& outSource,
			std::string& outDiagnostic)
		{
			std::string resolvedIncludes;
			for (const std::string& include : includes)
			{
				std::string contents;
				if (include.empty() || !reader(include, contents))
				{
					outDiagnostic = "Cannot resolve YAML shader include '" + include +
						"'. Include paths must be Content-root virtual paths.";
					return false;
				}

				resolvedIncludes += contents;
				resolvedIncludes += '\n';
			}

			outSource += resolvedIncludes;
			outDiagnostic.clear();
			return true;
		}
	};
}
