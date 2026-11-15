#include "CoopSharedDropDelivery.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using CoopSharedDropDelivery::Entry;
constexpr uint64_t kSession = 19;
int g_checks = 0;

void Check(bool condition, const char* name)
{
    ++g_checks;
    if (!condition)
    {
        std::cerr << "FAIL " << name << '\n';
        std::exit(1);
    }
}

Entry Row(uint64_t stableId = 23, uint32_t delivered = 0)
{
    return {kSession, stableId, 29, 3, delivered, 31, 37};
}

std::string Section(const std::vector<Entry>& rows, uint64_t session = kSession)
{
    std::ostringstream output;
    CoopSharedDropDelivery::WriteSection(output, rows, session);
    return output.str();
}

bool Parses(const std::string& text)
{
    std::vector<Entry> rows;
    bool present = false;
    return CoopSharedDropDelivery::ParseSection(text, rows, present);
}

void ParserChecks()
{
    const Entry row = Row();
    std::vector<Entry> rows;
    bool present = true;
    uint64_t session = 99;
    Check(CoopSharedDropDelivery::ParseSection("inventoryCount=0\n", rows, present, &session) &&
        !present && rows.empty() && session == 0, "legacy snapshot has no receipts");
    Check(CoopSharedDropDelivery::ParseSection(Section({}), rows, present, &session) &&
        present && rows.empty() && session == kSession, "empty section retains session fence");
    Check(CoopSharedDropDelivery::ParseSection(Section({row}), rows, present, &session) &&
        rows.size() == 1 && CoopSharedDropDelivery::SameCommit(rows.front(), row) &&
        rows.front().deliveredCount == row.deliveredCount, "receipt round trip");
    Entry wide = {UINT64_MAX, UINT64_MAX, UINT64_MAX, 2147483647u, 2147483647u, UINT32_MAX, UINT32_MAX};
    Check(Parses(Section({wide}, UINT64_MAX)), "unsigned identity and sequence limits survive round trip");
    Check(!Parses("sharedDropDeliveryCount=0\n"), "missing session rejected");
    Check(!Parses("sharedDropHostSessionId=19\n"), "missing count rejected");
    Check(!Parses(Section({row}) + "sharedDropDeliveryCount=1\n"), "duplicate count rejected");
    Check(!Parses(Section({row}) + "sharedDropHostSessionId=19\n"), "duplicate session rejected");
    Check(!Parses(Section({}, 0)), "zero session rejected");
    Check(!Parses("sharedDropHostSessionId=19\nsharedDropDeliveryCount=4097\n"), "oversized section rejected");
    Check(!Parses("sharedDropHostSessionId=19\nsharedDropDeliveryCount=-1\n"), "negative count rejected");
    Check(!Parses("sharedDropHostSessionId=18446744073709551616\nsharedDropDeliveryCount=0\n"),
        "uint64 overflow rejected");
    Check(!Parses("sharedDropHostSessionId=19 extra\nsharedDropDeliveryCount=0\n"),
        "trailing header junk rejected");
    Check(!Parses(Section({row, row})), "duplicate lifetime rejected");
    Check(!Parses(Section({}) + "sharedDropDelivery=19 23 29 3 0 31 37\n"), "count mismatch rejected");
    Check(!Parses("sharedDropDelivery=19 23 29 3 0 31 37\n"), "orphan row rejected");
    const std::vector<std::string> invalidRows = {
        "19 23 29 3 0 31", "19 23 29 3 0 31 37 trailing", "19 23 29 3 4 31 37",
        "19 23 29 0 0 31 37", "19 0 29 3 0 31 37", "0 23 29 3 0 31 37",
        "19 23 0 3 0 31 37", "19 23 29 3 0 0 37", "19 23 29 3 0 31 0",
        "19 23 29 2147483648 0 31 37", "19 23 29 3 0 4294967296 37",
        "19 23 29 3 0 31 4294967296", "19 -23 29 3 0 31 37", "19 +23 29 3 0 31 37",
        "19 0x23 29 3 0 31 37", "19 18446744073709551616 29 3 0 31 37"
    };
    for (const std::string& invalid : invalidRows)
        Check(!Parses("sharedDropHostSessionId=19\nsharedDropDeliveryCount=1\nsharedDropDelivery=" + invalid + "\n"),
            "malformed or out-of-range row rejected");
    Entry previous = row;
    previous.hostSessionId = kSession - 1;
    Check(Parses(Section({previous, row})), "same stable number in different timeline is distinct");
    Check(Parses("sharedDropHostSessionId=19\r\nsharedDropDeliveryCount=1\r\n"
        "sharedDropDelivery=19 23 29 3 0 31 37\r\n"), "CRLF snapshots parse");
    const std::string payload = "version=1\ninventoryCount=1\nitem=29 3\n" + Section({row}) + "health=100 100\n";
    Check(CoopSharedDropDelivery::WithoutSection(payload) ==
        "version=1\ninventoryCount=1\nitem=29 3\nhealth=100 100\n", "section rewrite preserves inventory payload");
    std::vector<Entry> maximum;
    for (size_t index = 0; index != CoopSharedDropDelivery::kMaxEntries; ++index)
        maximum.push_back(Row(100 + index));
    Check(CoopSharedDropDelivery::ParseSection(Section(maximum), rows, present, &session) &&
        rows.size() == CoopSharedDropDelivery::kMaxEntries && session == kSession,
        "exact maximum receipt table round trips");
    Check(!Parses(Section(maximum) + "sharedDropDelivery=19 99999 29 3 0 31 37\n"),
        "extra row beyond exact maximum rejected even with valid declared count");
}

void MergeChecks()
{
    const Entry pending = Row();
    std::vector<Entry> incoming;
    Check(CoopSharedDropDelivery::MergeForUpload(incoming, {pending}, kSession) && incoming.size() == 1 &&
        incoming.front().deliveredCount == 0, "precommit journal retains assigned undelivered entitlement");
    Check(CoopSharedDropDelivery::MergeForUpload(incoming, {pending}, kSession) && incoming.size() == 1,
        "repeated pending upload does not multiply entitlement");
    incoming = {Row(23, 2)};
    Check(CoopSharedDropDelivery::MergeForUpload(incoming, {Row(23, 1)}, kSession), "partial delivery can advance");
    incoming = {Row(23, 1)};
    Check(!CoopSharedDropDelivery::MergeForUpload(incoming, {Row(23, 2)}, kSession), "delivered count regression rejected");
    incoming.clear();
    Check(!CoopSharedDropDelivery::MergeForUpload(incoming, {Row(23, 2)}, kSession), "missing acquired receipt rejected");
    incoming = {Row()};
    Check(!CoopSharedDropDelivery::MergeForUpload(incoming, {}, kSession), "client cannot invent current entitlement");
    Entry historical = Row();
    historical.hostSessionId = kSession - 1;
    incoming = {historical};
    Check(CoopSharedDropDelivery::MergeForUpload(incoming, {}, kSession), "old session receipt remains inert");
    incoming.clear();
    historical.deliveredCount = 2;
    Check(CoopSharedDropDelivery::MergeForUpload(incoming, {historical}, kSession) && incoming.size() == 1,
        "old session receipt retained without activating entitlement");
    for (int field = 0; field != 4; ++field)
    {
        Entry conflicting = pending;
        if (field == 0) ++conflicting.archetypeId;
        if (field == 1) ++conflicting.count;
        if (field == 2) ++conflicting.transactionSequence;
        if (field == 3) ++conflicting.objectVersion;
        incoming = {conflicting};
        Check(!CoopSharedDropDelivery::MergeForUpload(incoming, {pending}, kSession), "commit metadata conflict rejected");
    }
    incoming = {Row(23, 3), Row(24, 3)};
    Check(CoopSharedDropDelivery::MergeForUpload(incoming, incoming, kSession) && incoming.size() == 2,
        "same archetype separate drops are not deduplicated");
    incoming.clear();
    for (size_t index = 0; index != CoopSharedDropDelivery::kMaxEntries; ++index)
        incoming.push_back(Row(100 + index));
    Entry historicalPending = Row(1);
    historicalPending.hostSessionId = kSession - 1;
    Check(CoopSharedDropDelivery::MergeForUpload(incoming, incoming, kSession + 1),
        "full unchanged section remains valid");
    Check(!CoopSharedDropDelivery::MergeForUpload(incoming, {historicalPending}, kSession + 1),
        "merge cannot exceed bounded entry limit");
}

void WeaponChecks()
{
    using namespace CoopWeaponState;
    const State source{true, 37.125f, 6, {{UINT64_MAX, 2}, {41, 3}}, true};
    Wire wire;
    State decoded;
    Check(Encode(source, wire) && Decode(wire, decoded) && Same(source, decoded),
        "weapon wire preserves exact condition ammo and upgrades");
    Check(sizeof(Wire) == 216, "weapon wire has stable fixed layout");
    std::swap(decoded.mods[0], decoded.mods[1]);
    Check(Same(source, decoded), "upgrade ordering is not identity");
    for (int field = 0; field != 10; ++field)
    {
        Wire invalid = wire;
        if (field == 0) invalid.present = 2;
        if (field == 1) invalid.condition = std::numeric_limits<float>::quiet_NaN();
        if (field == 2) invalid.condition = std::numeric_limits<float>::infinity();
        if (field == 3) invalid.ammoLoaded = -1;
        if (field == 4) invalid.modCount = kMaxMods + 1;
        if (field == 5) invalid.modIds[1] = invalid.modIds[0];
        if (field == 6) invalid.modLevels[0] = 17;
        if (field == 7) invalid.modIds[kMaxMods - 1] = 1;
        if (field == 8) invalid.present = 0;
        if (field == 9) invalid.ammoLooted = 2;
        Check(!Decode(invalid, decoded), "malformed weapon wire rejected");
    }
    State maximum{true, 100, 1000000, {}};
    for (size_t i = 0; i < kMaxMods; ++i)
        maximum.mods.emplace_back(i + 1, 16);
    Check(Encode(maximum, wire) && Decode(wire, decoded) && Same(maximum, decoded),
        "maximum bounded weapon metadata round trips");
    maximum.mods.emplace_back(99, 1);
    Check(!Encode(maximum, wire), "oversized native upgrade list rejected without truncation");

    Entry row = Row();
    row.weaponState = source;
    std::vector<Entry> rows;
    bool present = false;
    Check(CoopSharedDropDelivery::ParseSection(Section({row}), rows, present) &&
        rows.size() == 1 && Same(rows[0].weaponState, source),
        "receipt round trip retains exact weapon metadata");
    Entry changed = row;
    ++changed.weaponState.ammoLoaded;
    rows = {changed};
    Check(!CoopSharedDropDelivery::SameCommit(row, changed) &&
        !CoopSharedDropDelivery::MergeForUpload(rows, {row}, kSession),
        "upload cannot change committed weapon metadata");
    for (const char* suffix : {"w1 0 -1 0", "w1 0 0 17", "w1 0 0 1 42 0",
             "w1 0 0 2 42 1 42 2", "w1 0 0 0 extra", "w2 0 0 0"})
        Check(!ReadSuffix(suffix, decoded), "malformed weapon receipt rejected");
    Check(ReadSuffix("", decoded) && !decoded.present, "legacy receipt remains readable");

    struct Mods
    {
        std::map<uint64_t, int> m_weaponModIds;
        unsigned clears = 0, adds = 0;
        void Clear() { ++clears; m_weaponModIds.clear(); }
        void AddMod(uint64_t id) { ++adds; ++m_weaponModIds[id]; }
    };
    struct StatModifier { int m_StatName; float m_Modifier; };
    struct Level { std::vector<StatModifier> m_StatModifiers{{1, 2.0f}}; };
    struct Definition {
        std::string m_Name = "upgrade";
        Level level;
        const Level* GetLevelWeaponMod(int n) const { return n <= 3 ? &level : nullptr; }
    };
    struct Library {
        Definition definition;
        const Definition* GetWeaponModByID(uint64_t id) const { return id == 99 ? nullptr : &definition; }
    } library;
    struct Stats {
        float value = 10;
        void AddModifier(int, float delta, const std::string&) { value += delta; }
    };
    struct Weapon {
        float m_condition = 100;
        int m_numAmmoLoaded = 15;
        Mods m_weaponMods;
        bool m_bHasAmmoBeenLooted = false;
        Stats m_statsComponent;
        void InitializeStats() { m_statsComponent.value = 10; }
    } weapon;
    Check(ApplyNative(weapon, source, library), "native apply accepts known upgrade definitions");
    Check(Same(CaptureNative(weapon), source) && weapon.m_weaponMods.clears == 1 &&
        weapon.m_weaponMods.adds == 5, "native apply restores values and exact upgrade levels");
    Check(weapon.m_statsComponent.value == 20, "upgrade levels also restore functional native stats");
    ApplyNative(weapon, source, library);
    Check(weapon.m_weaponMods.clears == 1 && weapon.m_weaponMods.adds == 5,
        "repeated apply does not stack native upgrade effects");
    Check(weapon.m_statsComponent.value == 20, "repeated apply leaves functional stats unchanged");
    State unknown = source;
    unknown.mods = {{99, 1}};
    Check(!ApplyNative(weapon, unknown, library) && Same(CaptureNative(weapon), source),
        "unknown upgrade refuses apply before changing the weapon");
    unknown.mods = {{1, 4}};
    Check(!ApplyNative(weapon, unknown, library) && weapon.m_statsComponent.value == 20,
        "missing native upgrade level refuses partial restore");
    State unmodified{true, 50, 0, {}};
    ApplyNative(weapon, unmodified, library);
    Check(Same(CaptureNative(weapon), unmodified) && weapon.m_weaponMods.clears == 2,
        "unmodified source clears replica defaults");
    Check(weapon.m_statsComponent.value == 10, "removing upgrades resets their functional stats");
    weapon.m_bHasAmmoBeenLooted = true;
    ApplyNative(weapon, unmodified, library, false);
    Check(weapon.m_bHasAmmoBeenLooted, "legacy sidecar apply does not overwrite unknown loot state");
}
}

int main()
{
    ParserChecks();
    MergeChecks();
    WeaponChecks();
    std::cout << "Shared-drop receipt parser and upload merge: " << g_checks << " checks passed\n";
}
