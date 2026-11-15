#include "CoopSharedDropDelivery.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

class ModMain
{
public:
    struct PlayerSidecarState
    {
        bool hasInventory = true;
        bool hasSharedDropDeliveries = true;
        bool hasNativeCapture = true;
        uint64_t sharedDropHostSessionId = 19;
        std::vector<CoopSharedDropDelivery::Entry> sharedDropDeliveries;
        std::vector<int> inventory;
        int snapshotTag = 0;
    };
    bool ReconcileRecoveryPlayerStateSnapshot(PlayerSidecarState&, const PlayerSidecarState&);
};

#include "SharedDropRecoverySelectionActual.inc"

namespace
{
int checks = 0;
void Check(bool condition, const char* name)
{
    ++checks;
    if (!condition)
    {
        std::cerr << "FAIL " << name << '\n';
        std::exit(1);
    }
}

CoopSharedDropDelivery::Entry Row(uint64_t stable = 23, uint32_t delivered = 0)
{
    return {19, stable, 29, 3, delivered, 31, 37};
}

ModMain::PlayerSidecarState Snapshot(int tag, std::vector<CoopSharedDropDelivery::Entry> rows)
{
    ModMain::PlayerSidecarState state;
    state.snapshotTag = tag;
    state.inventory = {tag};
    state.sharedDropDeliveries = std::move(rows);
    return state;
}
}

int main()
{
    ModMain mod;
    {
        auto local = Snapshot(10, {});
        auto host = Snapshot(90, {Row()});
        Check(mod.ReconcileRecoveryPlayerStateSnapshot(local, host) && local.inventory == std::vector<int>{10} &&
            local.sharedDropDeliveries.size() == 1 && local.sharedDropDeliveries[0].deliveredCount == 0,
            "commit absent local baseline carries Host entitlement without changing inventory");
        Check(local.hasSharedDropDeliveries && local.sharedDropHostSessionId == 19 && !local.hasNativeCapture,
            "merged plain snapshot disables stale native replacement");
    }
    {
        auto local = Snapshot(10, {});
        const auto host = Snapshot(90, {Row(23, 3)});
        Check(mod.ReconcileRecoveryPlayerStateSnapshot(local, host) && local.inventory == host.inventory &&
            local.snapshotTag == host.snapshotTag && local.sharedDropDeliveries[0].deliveredCount == 3,
            "newer Host acquired receipt selects complete Host snapshot");
    }
    {
        auto local = Snapshot(10, {Row(23, 1)});
        const auto host = Snapshot(90, {Row(23, 2)});
        Check(mod.ReconcileRecoveryPlayerStateSnapshot(local, host) && local.inventory == host.inventory &&
            local.sharedDropDeliveries[0].deliveredCount == 2, "partial Host receipt selects paired inventory");
    }
    {
        auto local = Snapshot(10, {Row(23, 2)});
        const auto host = Snapshot(90, {Row(23, 1)});
        Check(mod.ReconcileRecoveryPlayerStateSnapshot(local, host) && local.snapshotTag == 10 &&
            local.sharedDropDeliveries[0].deliveredCount == 2, "newer local receipt retains paired local inventory");
    }
    {
        auto local = Snapshot(10, {Row(23, 2)});
        const auto host = Snapshot(90, {Row(23, 2)});
        Check(mod.ReconcileRecoveryPlayerStateSnapshot(local, host) && local.snapshotTag == 10 &&
            local.sharedDropDeliveries.size() == 1, "matching receipt is not appended twice");
    }
    {
        auto old = Row();
        old.hostSessionId = 18;
        auto local = Snapshot(10, {old});
        local.sharedDropHostSessionId = 18;
        const auto host = Snapshot(90, {Row(24)});
        Check(mod.ReconcileRecoveryPlayerStateSnapshot(local, host) && local.sharedDropHostSessionId == 19 &&
            local.sharedDropDeliveries.size() == 1 && local.sharedDropDeliveries[0].stableSpawnId == 24,
            "old timeline pending ownership does not leak into selected snapshot");
    }
    {
        auto local = Snapshot(10, {Row(23, 1)});
        const auto host = Snapshot(90, {Row(24)});
        Check(mod.ReconcileRecoveryPlayerStateSnapshot(local, host) && local.snapshotTag == 10 &&
            local.sharedDropDeliveries.size() == 2 && local.sharedDropDeliveries[0].deliveredCount == 1,
            "Host pending assignment preserves unrelated acquired local receipt");
    }
    for (int mismatch = 0; mismatch != 3; ++mismatch)
    {
        auto local = Snapshot(10, {Row()});
        auto host = Snapshot(90, {Row()});
        if (mismatch == 0) ++host.sharedDropDeliveries[0].archetypeId;
        if (mismatch == 1) ++host.sharedDropDeliveries[0].objectVersion;
        if (mismatch == 2) ++host.sharedDropDeliveries[0].transactionSequence;
        Check(!mod.ReconcileRecoveryPlayerStateSnapshot(local, host) && local.snapshotTag == 10 &&
            local.sharedDropDeliveries[0].archetypeId == 29, "conflicting committed metadata rejects without partial selection");
    }
    {
        auto local = Snapshot(10, {});
        for (size_t i = 0; i != CoopSharedDropDelivery::kMaxEntries; ++i)
            local.sharedDropDeliveries.push_back(Row(100 + i));
        const auto host = Snapshot(90, {Row(23)});
        Check(!mod.ReconcileRecoveryPlayerStateSnapshot(local, host) &&
            local.sharedDropDeliveries.size() == CoopSharedDropDelivery::kMaxEntries,
            "pending union remains bounded and fails without partial mutation");
    }
    {
        auto local = Snapshot(10, {});
        auto host = Snapshot(90, {});
        host.hasSharedDropDeliveries = false;
        Check(mod.ReconcileRecoveryPlayerStateSnapshot(local, host) && local.snapshotTag == 10,
            "legacy Host snapshot does not overwrite recovery state");
        host.hasSharedDropDeliveries = true;
        host.sharedDropHostSessionId = 0;
        Check(!mod.ReconcileRecoveryPlayerStateSnapshot(local, host), "explicit Host section requires session fence");
        host.sharedDropHostSessionId = 19;
        local.hasInventory = false;
        Check(!mod.ReconcileRecoveryPlayerStateSnapshot(local, host), "receipt selection requires paired inventory");
    }
    std::cout << checks << " recovery snapshot selection checks passed\n";
}
