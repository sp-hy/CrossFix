#pragma once
#include <Windows.h>

// Helper function to write memory with proper protection
bool WriteMemory(uintptr_t address, const void* data, size_t size);
