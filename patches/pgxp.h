#pragma once
#include <Windows.h>
#include <cstdint>

class Settings;

// PGXP for RDE's software GTE and GP0 path: fine vertices kept at RDE's 1/8
// pixel, precise sub-pixel vertex positions from the full-precision projection
// (with unrounded skinning and matrix composition), precise NCLIP culling, and
// the per-vertex depth the replacement polygon shaders use for
// perspective-correct texturing. F6 switches it off and on at run time.

// pgxp_enabled in settings.ini (on when the key is missing)
bool PgxpEnabled(const Settings &settings);

bool ApplyPgxpPatch(uintptr_t base);
