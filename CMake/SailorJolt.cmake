# Jolt's STL exception policy is private to Jolt. Sailor and its clients must
# use the same std::exception type across the DLL boundary.
get_target_property(SAILOR_JOLT_COMPILE_DEFINITIONS Jolt::Jolt INTERFACE_COMPILE_DEFINITIONS)
list(FILTER SAILOR_JOLT_COMPILE_DEFINITIONS EXCLUDE REGEX "_HAS_EXCEPTIONS=0")
set_property(TARGET Jolt::Jolt PROPERTY INTERFACE_COMPILE_DEFINITIONS
	"${SAILOR_JOLT_COMPILE_DEFINITIONS}")
unset(SAILOR_JOLT_COMPILE_DEFINITIONS)
