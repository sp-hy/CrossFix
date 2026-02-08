#pragma once
#include <cstdint>

// Apply save selector patch: scales 2-byte position values at specific
// addresses by the aspect ratio. aspectRatioMultiplier must remain valid
// (e.g. from GetAspectRatioMultiplierPtr()).
bool ApplySaveSelectorPatch(uintptr_t base, const float *aspectRatioMultiplier);
