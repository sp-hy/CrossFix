#include "battle.h"
#include <iostream>

static uintptr_t g_inBattleAddr = 0;

bool ApplyBattlePatch(uintptr_t base) {
	g_inBattleAddr = base + 0x6A1389;
	std::cout << "Battle patch system initialized" << std::endl;
	return true;
}

bool IsInBattle() {
	if (g_inBattleAddr == 0) return false;
	
	try {
		return (*(unsigned char*)g_inBattleAddr == 1);
	} catch (...) {
		return false;
	}
}
