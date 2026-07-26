#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <Prey/CryMath/Cry_Math.h>

struct NativeCapturedItemState
{
    unsigned itemId = 0;
    std::uintptr_t itemPtr = 0;
    uint64_t archetypeId = 0;
    int count = 0;
    unsigned ownerId = 0;
    int x = -1;
    int y = -1;
    int width = -1;
    int height = -1;
    int category = 0;
    uint32_t flags = 0;
    bool hasCell = false;
    bool isWeapon = false;
    float weaponCondition = 0.0f;
    int weaponAmmoLoaded = 0;
    int weaponAmmoCount = -1;
    uint32_t weaponModCount = 0;
    uint32_t weaponModTotalLevel = 0;
    std::vector<std::pair<uint64_t, int>> weaponMods;
};

struct NativeSideBlobCaptureState
{
    bool active = false;
    bool sawPlayerWrite = false;
    bool sawInventoryWrite = false;
    bool sawItemWrite = false;
    std::string reason;
    std::string username;
    std::string levelName;
    uint32_t worldEpoch = 0;
    Vec3 position = Vec3(ZERO);
    Quat rotation = Quat::CreateIdentity();
    Quat viewRotation = Quat::CreateIdentity();
    float health = 0.0f;
    float maxHealth = 0.0f;
    float psiPoints = 0.0f;
    float psiMaxPoints = 0.0f;
    int armor = 0;
    int maxArmor = 0;
    int stance = 0;
    int selectedPsiPower = 15;
    int equippedPsiPower = 15;
    unsigned inventoryOwnerId = 0;
    int inventoryWidth = 0;
    int inventoryHeight = 0;
    unsigned equippedWeaponId = 0;
    unsigned lastEquippedWeaponId = 0;
    unsigned backupWeaponId = 0;
    unsigned toBeEquippedWeaponId = 0;
    uint32_t weaponCount = 0;
    uint32_t specialWeaponCount = 0;
    uint32_t itemSerializeHits = 0;
    uint32_t checksum = 0;
    bool hasNativeFragmentPayload = false;
    uint16_t nativeFragmentPayloadVersion = 0;
    uint32_t nativeFragmentPayloadBytes = 0;
    uint32_t nativeFragmentPayloadChecksum = 0;
    uint32_t nativeFragmentPayloadRawChecksum = 0;
    uint32_t nativeFragmentPayloadRanges = 0;
    uint32_t nativeFragmentPayloadInventoryRanges = 0;
    uint32_t nativeFragmentPayloadItemGroups = 0;
    uint32_t nativeFragmentPayloadItemRanges = 0;
    uint32_t nativeFragmentPayloadRawBytes = 0;
    uint64_t nativeFragmentPayloadRunId = 0;
    uint64_t nativeFragmentPayloadSchemaHash = 0;
    uint64_t nativeFragmentPayloadContentHash = 0;
    std::vector<uint8_t> nativeFragmentPayload;
    bool hasNativeSnapshotSave = false;
    uint32_t nativeSnapshotSaveBytes = 0;
    uint32_t nativeSnapshotSaveChecksum = 0;
    std::vector<uint8_t> nativeSnapshotSave;
    std::vector<NativeCapturedItemState> items;
};
