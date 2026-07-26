#pragma once

#include <cstdint>
#include <string>

namespace CoopEnemyMovementGate
{
// Remote observers never run a second movement planner. Their body follows
// the authority transform and exact native action stream instead.
bool ShouldBlockObserverNativeMovement(uint32_t flags);
bool RunSelfTest(std::string& detail);
}
