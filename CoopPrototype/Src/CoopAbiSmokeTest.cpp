#include "CommonModPch.h"

#include <Prey/GameDll/ark/turret/ArkTurret.h>

static_assert(sizeof(void*) == 8, "Prey Coop requires a 64-bit target ABI.");
static_assert(sizeof(ArkNpcUtils::Range) == 8, "ArkNpcUtils::Range ABI mismatch.");
static_assert(sizeof(ArkTurretAnimFsm::Fsm) == 80, "ArkTurretAnimFsm::Fsm ABI mismatch.");
static_assert(sizeof(ArkTurretDamageFsm::Fsm) == 56, "ArkTurretDamageFsm::Fsm ABI mismatch.");
static_assert(sizeof(ArkTurret) == 3720, "ArkTurret ABI mismatch.");

int main() noexcept
{
	return 0;
}
