#pragma once
#include <cstdint>

// Apply the voices patch: plays mods voice-pack MP3s for field and battle
// dialogue pages, stopping them when the window closes. volume 0..100 is a
// linear gain on the lines.
bool ApplyVoicesPatch(uintptr_t base, int volume);
