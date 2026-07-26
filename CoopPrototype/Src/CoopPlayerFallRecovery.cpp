#include "ModMain.h"
#include "CoopRuntimeGuards.h"
#include "CoopRuntimeLog.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CryPhysics/physinterface.h>
#include <Prey/CrySystem/ITimer.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>

namespace
{
using CoopRuntimeGuards::TryGuardedCall;

constexpr float kGroundConfirmationSeconds = 0.35f;
constexpr float kGroundMaxVerticalSpeed = 1.5f;
constexpr float kNormalRecoveryAirSeconds = 2.0f;
constexpr float kNormalRecoveryDropMeters = 30.0f;
constexpr float kNormalRecoveryDownSpeed = -6.0f;
constexpr float kEmergencyRecoveryDropMeters = 80.0f;
constexpr float kRecoveryCooldownSeconds = 2.0f;

struct FallRecoveryPolicyInput
{
    bool safeValid = false;
    bool sameLevel = false;
    bool zeroG = false;
    bool inAir = false;
    bool currentPositionValid = true;
    float airSeconds = 0.0f;
    float dropMeters = 0.0f;
    float verticalSpeed = 0.0f;
    float cooldownSeconds = 0.0f;
};

enum class FallRecoveryMovementClass
{
    Excluded,
    Grounded,
    Airborne,
};

FallRecoveryMovementClass ClassifyFallRecoveryMovementState(EArkPlayerMovementStateId state)
{
    switch (state)
    {
    case EArkPlayerMovementStateId::ground:
        return FallRecoveryMovementClass::Grounded;
    case EArkPlayerMovementStateId::jump:
    case EArkPlayerMovementStateId::fall:
    case EArkPlayerMovementStateId::death:
    case EArkPlayerMovementStateId::deathByRecyclerGrenade:
        return FallRecoveryMovementClass::Airborne;
    default:
        return FallRecoveryMovementClass::Excluded;
    }
}

bool IsFinitePosition(const Vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
        std::fabs(value.x) < 100000.0f &&
        std::fabs(value.y) < 100000.0f &&
        std::fabs(value.z) < 100000.0f;
}

bool ShouldRecoverFall(const FallRecoveryPolicyInput& input)
{
    if (!input.safeValid || !input.sameLevel || input.zeroG || !input.inAir || input.cooldownSeconds > 0.0f)
        return false;

    if (!input.currentPositionValid)
        return true;

    const bool emergencyDrop = input.dropMeters >= kEmergencyRecoveryDropMeters;
    const bool sustainedVoidFall =
        input.airSeconds >= kNormalRecoveryAirSeconds &&
        input.dropMeters >= kNormalRecoveryDropMeters &&
        input.verticalSpeed <= kNormalRecoveryDownSpeed;
    return emergencyDrop || sustainedVoidFall;
}

bool ReadPlayerVerticalSpeed(IEntity& entity, float& outSpeed)
{
    outSpeed = 0.0f;
    IPhysicalEntity* physics = entity.GetPhysics();
    if (!physics)
        return false;

    pe_status_dynamics dynamics;
    const int result = physics->GetStatus(&dynamics);
    if (result <= 0 || !std::isfinite(dynamics.v.z))
        return false;

    outSpeed = dynamics.v.z;
    return true;
}
} // namespace

void ModMain::ResetLocalPlayerFallRecovery(const char* reason)
{
    m_fallRecoverySafePosition = Vec3(ZERO);
    m_fallRecoverySafeRotation = Quat::CreateIdentity();
    m_fallRecoveryLastObservedPosition = Vec3(ZERO);
    m_fallRecoveryTrackingLevel.clear();
    m_fallRecoveryTrackingLevelEpoch = 0;
    m_fallRecoveryGroundedSeconds = 0.0f;
    m_fallRecoveryAirSeconds = 0.0f;
    m_fallRecoveryCooldownSeconds = 0.0f;
    m_fallRecoveryLastDropMeters = 0.0f;
    m_fallRecoveryLastVerticalSpeed = 0.0f;
    m_fallRecoveryMovementStateId = static_cast<int>(EArkPlayerMovementStateId::null);
    m_fallRecoverySafeValid = false;
    m_fallRecoveryHasLastObservedPosition = false;
    m_lastFallRecoveryEvent = reason && reason[0] ? reason : "fall recovery reset";
}

void ModMain::TickLocalPlayerFallRecovery(float frameTime)
{
    const float dt = std::clamp(std::max(frameTime, 0.0f), 0.0f, 0.1f);
    m_fallRecoveryCooldownSeconds = std::max(0.0f, m_fallRecoveryCooldownSeconds - dt);

    if (m_networkMode == CoopNetworkMode::Off ||
        !IsSessionGameplayReady() ||
        m_saveLoadGuardActive ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        !ArkPlayer::GetInstancePtr())
    {
        m_fallRecoveryGroundedSeconds = 0.0f;
        m_fallRecoveryAirSeconds = 0.0f;
        m_fallRecoveryHasLastObservedPosition = false;
        return;
    }

    ArkPlayer& player = ArkPlayer::GetInstance();
    IEntity* entity = player.GetEntity();
    if (!entity)
        return;

    const std::string currentLevel = GetCurrentLevelName();
    if (currentLevel.empty() || currentLevel == "unknown")
        return;
    if (m_fallRecoveryTrackingLevel != currentLevel ||
        m_fallRecoveryTrackingLevelEpoch != m_localLevelEpoch)
    {
        ResetLocalPlayerFallRecovery("fall recovery level boundary");
        m_fallRecoveryTrackingLevel = currentLevel;
        m_fallRecoveryTrackingLevelEpoch = m_localLevelEpoch;
    }

    // The main lift passenger helper supplies a stronger moving-ground
    // contract than the player's airborne FSM. Treat its corrected cabin
    // position as the current safe point instead of recovering to the floor
    // that the lift already left.
    if (m_mainLiftPassengerAttached)
    {
        const Vec3 passengerPosition = entity->GetWorldPos();
        const Quat passengerRotation = entity->GetWorldRotation();
        if (IsFinitePosition(passengerPosition))
        {
            m_fallRecoverySafePosition = passengerPosition;
            m_fallRecoverySafeRotation = passengerRotation;
            m_fallRecoverySafeValid = true;
            m_fallRecoveryLastObservedPosition = passengerPosition;
            m_fallRecoveryHasLastObservedPosition = true;
        }
        m_fallRecoveryGroundedSeconds = 0.0f;
        m_fallRecoveryAirSeconds = 0.0f;
        m_fallRecoveryLastDropMeters = 0.0f;
        m_fallRecoveryLastVerticalSpeed = 0.0f;
        return;
    }

    std::string reason;
    Vec3 currentPosition = Vec3(ZERO);
    Quat currentRotation = Quat::CreateIdentity();
    bool zeroG = false;
    bool dead = false;
    TryGuardedCall("fall recovery player position", [entity]() { return entity->GetWorldPos(); }, currentPosition, &reason);
    TryGuardedCall("fall recovery player rotation", [entity]() { return entity->GetWorldRotation(); }, currentRotation, &reason);
    TryGuardedCall("fall recovery IsZeroG", [&player]() { return player.IsZeroG(); }, zeroG, &reason);
    TryGuardedCall("fall recovery IsDead", [&player]() { return player.IsDead(); }, dead, &reason);
    currentRotation.Normalize();

    const EArkPlayerMovementStateId movementState = player.m_movementFSM.m_currentStateId;
    m_fallRecoveryMovementStateId = static_cast<int>(movementState);
    const FallRecoveryMovementClass movementClass = ClassifyFallRecoveryMovementState(movementState);
    const bool inAir = movementClass == FallRecoveryMovementClass::Airborne;

    float verticalSpeed = 0.0f;
    const bool physicsSpeedValid = ReadPlayerVerticalSpeed(*entity, verticalSpeed);
    if (!physicsSpeedValid && m_fallRecoveryHasLastObservedPosition && dt > 0.001f && IsFinitePosition(currentPosition))
        verticalSpeed = (currentPosition.z - m_fallRecoveryLastObservedPosition.z) / dt;
    m_fallRecoveryLastVerticalSpeed = verticalSpeed;

    const bool currentPositionValid = IsFinitePosition(currentPosition);
    if (currentPositionValid)
    {
        m_fallRecoveryLastObservedPosition = currentPosition;
        m_fallRecoveryHasLastObservedPosition = true;
    }

    // The character controller does not expose useful contact counts for the
    // player capsule. The native movement FSM is the authoritative ground
    // oracle; special movement states must not consume a gravity-area safe point.
    if (zeroG || movementClass == FallRecoveryMovementClass::Excluded)
    {
        m_fallRecoveryGroundedSeconds = 0.0f;
        m_fallRecoveryAirSeconds = 0.0f;
        m_fallRecoveryLastDropMeters = 0.0f;
        return;
    }

    if (movementClass == FallRecoveryMovementClass::Grounded &&
        !dead && !m_localPlayerDowned && currentPositionValid &&
        std::fabs(verticalSpeed) <= kGroundMaxVerticalSpeed &&
        m_fallRecoveryCooldownSeconds <= 0.0f)
    {
        m_fallRecoveryAirSeconds = 0.0f;
        m_fallRecoveryLastDropMeters = 0.0f;
        m_fallRecoveryGroundedSeconds += dt;
        if (m_fallRecoveryGroundedSeconds >= kGroundConfirmationSeconds)
        {
            const bool distinctCapture = !m_fallRecoverySafeValid ||
                (currentPosition - m_fallRecoverySafePosition).GetLengthSquared() >= 1.0f;
            m_fallRecoverySafePosition = currentPosition;
            m_fallRecoverySafeRotation = currentRotation;
            m_fallRecoverySafeValid = true;
            if (distinctCapture)
                ++m_fallRecoverySafeCaptures;
            m_lastFallRecoveryEvent =
                "ground confirmed level=" + currentLevel +
                " pos=" + std::to_string(currentPosition.x) + "," +
                    std::to_string(currentPosition.y) + "," +
                    std::to_string(currentPosition.z);
        }
        return;
    }

    m_fallRecoveryGroundedSeconds = 0.0f;
    if (movementClass != FallRecoveryMovementClass::Airborne)
    {
        m_fallRecoveryAirSeconds = 0.0f;
        return;
    }

    m_fallRecoveryAirSeconds += dt;
    m_fallRecoveryLastDropMeters =
        m_fallRecoverySafeValid && currentPositionValid
            ? m_fallRecoverySafePosition.z - currentPosition.z
            : 0.0f;
    const FallRecoveryPolicyInput input {
        m_fallRecoverySafeValid,
        m_fallRecoveryTrackingLevel == currentLevel &&
            m_fallRecoveryTrackingLevelEpoch == m_localLevelEpoch,
        zeroG,
        inAir,
        currentPositionValid,
        m_fallRecoveryAirSeconds,
        m_fallRecoveryLastDropMeters,
        verticalSpeed,
        m_fallRecoveryCooldownSeconds,
    };
    if (!ShouldRecoverFall(input))
    {
        if (m_fallRecoverySafeValid && m_fallRecoveryAirSeconds >= kNormalRecoveryAirSeconds)
            ++m_fallRecoveryPolicyRejects;
        return;
    }

    const Vec3 recoveryPosition = m_fallRecoverySafePosition;
    const Quat recoveryRotation = m_fallRecoverySafeRotation;
    const float recoveredDrop = m_fallRecoveryLastDropMeters;
    const float recoveredAirSeconds = m_fallRecoveryAirSeconds;
    if (!TeleportLocalPlayer(recoveryPosition, recoveryRotation))
    {
        m_lastFallRecoveryEvent = "fall recovery teleport failed";
        return;
    }

    ApplyEntityPhysicsVelocity(*entity, Vec3(ZERO));
    if (dead || m_localPlayerDowned)
        ReviveLocalPlayer(std::max(m_reviveHealth, 10.0f), true);
    SendLocalPlayerStatus(
        CoopProtocol::kPlayerStatusFlagTeleport,
        4,
        &recoveryPosition,
        &recoveryRotation);
    m_hasLastTransmittedPosePacket = false;
    m_networkTickAccumulator = std::max(m_networkTickAccumulator, 1.0f);
    m_fallRecoveryLastObservedPosition = recoveryPosition;
    m_fallRecoveryHasLastObservedPosition = true;
    m_fallRecoveryAirSeconds = 0.0f;
    m_fallRecoveryGroundedSeconds = 0.0f;
    m_fallRecoveryCooldownSeconds = kRecoveryCooldownSeconds;
    ++m_fallRecoveryRecoveries;
    m_lastFallRecoveryEvent =
        "recovered void fall drop=" + std::to_string(recoveredDrop) +
        " air=" + std::to_string(recoveredAirSeconds) +
        " speed=" + std::to_string(verticalSpeed) +
        " pos=" + std::to_string(recoveryPosition.x) + "," +
            std::to_string(recoveryPosition.y) + "," +
            std::to_string(recoveryPosition.z);
    CoopRuntimeLog::Write(m_lastFallRecoveryEvent);
}

bool ModMain::DebugLocalPlayerFallRecoverySelfTest(std::string& detail)
{
    struct Case
    {
        const char* name;
        FallRecoveryPolicyInput input;
        bool expected;
    };
    const Case cases[] = {
        {"no_safe", {false, true, false, true, true, 3.0f, 40.0f, -9.0f, 0.0f}, false},
        {"zero_g", {true, true, true, true, true, 3.0f, 90.0f, -9.0f, 0.0f}, false},
        {"shallow_fall", {true, true, false, true, true, 3.0f, 12.0f, -9.0f, 0.0f}, false},
        {"sustained_void", {true, true, false, true, true, 2.1f, 31.0f, -7.0f, 0.0f}, true},
        {"emergency_drop", {true, true, false, true, true, 0.1f, 81.0f, -1.0f, 0.0f}, true},
        {"cooldown", {true, true, false, true, true, 4.0f, 90.0f, -12.0f, 0.5f}, false},
        {"invalid_position", {true, true, false, true, false, 0.1f, 0.0f, 0.0f, 0.0f}, true},
    };

    unsigned passed = 0;
    for (const Case& test : cases)
        passed += ShouldRecoverFall(test.input) == test.expected ? 1u : 0u;

    struct MovementCase
    {
        EArkPlayerMovementStateId state;
        FallRecoveryMovementClass expected;
    };
    const MovementCase movementCases[] = {
        {EArkPlayerMovementStateId::ground, FallRecoveryMovementClass::Grounded},
        {EArkPlayerMovementStateId::jump, FallRecoveryMovementClass::Airborne},
        {EArkPlayerMovementStateId::fall, FallRecoveryMovementClass::Airborne},
        {EArkPlayerMovementStateId::death, FallRecoveryMovementClass::Airborne},
        {EArkPlayerMovementStateId::zerog, FallRecoveryMovementClass::Excluded},
        {EArkPlayerMovementStateId::slide, FallRecoveryMovementClass::Excluded},
    };
    for (const MovementCase& test : movementCases)
        passed += ClassifyFallRecoveryMovementState(test.state) == test.expected ? 1u : 0u;

    const size_t totalCases = std::size(cases) + std::size(movementCases);
    const bool ok = passed == totalCases;
    if (ok)
        ++m_fallRecoverySelfTestPasses;
    detail =
        "fall recovery selftest passed=" + std::to_string(passed) +
        "/" + std::to_string(totalCases) +
        " groundedConfirm=" + std::to_string(kGroundConfirmationSeconds) +
        " normal=" + std::to_string(kNormalRecoveryAirSeconds) + "s/" +
            std::to_string(kNormalRecoveryDropMeters) + "m" +
        " emergency=" + std::to_string(kEmergencyRecoveryDropMeters) + "m";
    m_lastFallRecoveryEvent = detail;
    return ok;
}
