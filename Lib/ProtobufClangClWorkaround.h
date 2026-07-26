#pragma once

// Protobuf 5.29.3 selects constinit for clang-cl even though imported
// protobuf constructors cannot be constant-evaluated with the MSVC ABI.
// Keep the feature-test macro environment identical for every translation
// unit that includes the generated protocol header.
#if defined(__clang__)
#include "absl/base/attributes.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbuiltin-macro-redefined"
#undef __cpp_constinit
#pragma clang diagnostic pop

#undef ABSL_HAVE_CPP_ATTRIBUTE
#define ABSL_HAVE_CPP_ATTRIBUTE(attribute) 0
#endif
