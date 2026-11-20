#pragma once
#include <cstdint>

// Apply the voices patch: plays mods voice-pack MP3s for field and battle
// dialogue pages, stopping them when the window closes.
bool ApplyVoicesPatch(uintptr_t base);
