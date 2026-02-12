#pragma once
#include <cstdint>

// Apply the voices patch: hook dialog text loading and stub voice playback.
// Monitors dialog buffer and pagination to decide when to play/cut voice files.
bool ApplyVoicesPatch(uintptr_t base);

// Stub: log the voice file that would be played (no actual playback yet).
void PlayVoiceFile(const char *filename);
