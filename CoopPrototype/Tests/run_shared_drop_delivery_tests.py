#!/usr/bin/env python3
"""Exercise unmodified native delivery methods against deterministic engine fakes."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile


root = Path(__file__).resolve().parents[1]
source = root / "Src/CoopSharedDropDelivery.cpp"
text = source.read_text()
actual = text[text.index("namespace\n{"):]
sync = (root / "Src/CoopSharedDropSync.cpp").read_text()
sync_actual = sync[sync.index("void ModMain::RestoreCommittedSharedDropDeliveries("):sync.index("void ModMain::TickSharedDropPickupRetries(")]
sync_actual += sync[sync.index("bool ModMain::ShouldSuppressRecyclerForSharedDrop("):]
header = (root / "Src/ModMain.h").read_text()
record = header[header.index("    struct SharedDropRecord\n"):header.index("    uint64_t BuildSharedDropStableId(")]
main = (root / "Src/ModMain.cpp").read_text()
drop_hook_start = main.index("static void CArkItem_Drop_Hook(")
drop_hook = main[drop_hook_start:main.index("\nstatic bool CArkItem_PickUp_Hook(", drop_hook_start)]
drop_native = "s_hookCArkItemDrop.InvokeOrig(item, dropCount, altPosition);"
drop_dirty = 'gMod->MarkLocalInventoryDirty("CArkItem::Drop");'
drop_publish = 'gMod->OnNativeSharedItemDropped(droppedItem, dropCount, "CArkItem::Drop");'
drop_post_native = "    if (gMod && localPlayerDrop)\n    {\n"
if (
    drop_hook.count(drop_native) != 1
    or drop_hook.count(drop_dirty) != 1
    or drop_hook.count(drop_publish) != 1
    or drop_post_native not in drop_hook[drop_hook.index(drop_native):]
):
    raise RuntimeError("local native Drop must mark inventory dirty after native completion")
drop_local_block_start = drop_hook.index(drop_post_native, drop_hook.index(drop_native))
drop_local_block = drop_hook[drop_local_block_start:drop_hook.index("\n    }", drop_local_block_start)]
if (
    drop_dirty not in drop_local_block
    or drop_publish not in drop_local_block
    or not drop_local_block.index(drop_dirty) < drop_local_block.index(drop_publish)
    or not drop_hook.index("inventory->Contains(item->GetEntityId())") < drop_hook.index(drop_native)
):
    raise RuntimeError("Drop dirty marking must use pre-drop inventory membership before publishing")
weapon_hook_start = main.index("static bool CArkWeapon_PickUp_Hook(")
weapon_hook = main[weapon_hook_start:main.index("\n}", weapon_hook_start)]
if (
    main.count("s_hookCArkWeaponPickUp = CArkWeapon::FPickUp.MakeHook();") != 1
    or main.count("s_hookCArkWeaponPickUp.SetHookFunc(&CArkWeapon_PickUp_Hook);") != 1
    or "CArkItem* item" not in weapon_hook
    or weapon_hook.count("ShouldDeferNativeSharedItemPickup(") != 1
    or weapon_hook.count("s_hookCArkWeaponPickUp.InvokeOrig(") != 1
    or not weapon_hook.index("ShouldDeferNativeSharedItemPickup(")
        < weapon_hook.index("return false;")
        < weapon_hook.index("s_hookCArkWeaponPickUp.InvokeOrig(")
):
    raise RuntimeError("weapon PickUp override bypasses the central pre-native transaction policy")
if actual.count("bool ModMain::TryDeliverCommittedSharedDrop(") != 1:
    raise RuntimeError("production native delivery body missing or duplicated")
print("source_sha256=" + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
print("selected_methods_sha256=" + hashlib.sha256(actual.encode()).hexdigest(), flush=True)
print("selected_sync_methods_sha256=" + hashlib.sha256(sync_actual.encode()).hexdigest(), flush=True)
print("local_drop_dirty_source_wiring=post_native_inventory_member_only", flush=True)
print("weapon_pickup_entry_wiring=central_policy_before_native", flush=True)
with tempfile.TemporaryDirectory(prefix="prey-drop-delivery-tests-") as directory:
    build = Path(directory)
    (build / "SharedDropDeliveryActual.inc").write_text(actual)
    (build / "SharedDropDeliverySyncActual.inc").write_text(sync_actual)
    (build / "SharedDropRecordActual.inc").write_text(record)
    binary = build / "shared-drop-delivery-tests"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(build), "-I", str(root / "Src"),
        str(root / "Tests/CoopSharedDropDeliveryTests.cpp"), "-o", str(binary),
    ], check=True)
    sys.exit(subprocess.run([str(binary)]).returncode)
