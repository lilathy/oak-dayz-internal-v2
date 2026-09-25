#pragma once

#include <string>

// Applies a complete, signed runtimeValues map atomically. No offset is changed
// unless every required value is present and passes conservative range checks.
bool OakApplyRuntimeOffsets(const std::string& verifiedPackagePayload, bool apply);
