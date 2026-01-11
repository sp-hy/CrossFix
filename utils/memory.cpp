#include "memory.h"

// Helper function to write memory with proper protection
bool WriteMemory(uintptr_t address, const void* data, size_t size) {
	DWORD oldProtect;
	if (!VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
		return false;
	}
	memcpy((void*)address, data, size);
	VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
	return true;
}
