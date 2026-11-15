# Shared-Drop Overflow Control-Flow Audit

## Scope and Provenance

- Read-only native-code/source audit, 2026-11-09. No gameplay result is claimed here.
- Exact PR60 source: `070cb0bae35aec382acca0b6511f8642d41d5f15`, compared with base `1419eaaf1fd441b37d2a995040bca21005e59c9f`.
- Base already includes #59 (`24e4021`, "Ignore cancelled overflow pickup drops").
- Native module: `/home/null234/Downloads/prey/_ghidra-input/PreyDll.dll`, x86-64 PE, preferred image base `0x180000000`, PE timestamp `2021-08-19 01:32:44 UTC`.
- Native SHA-256: `0485c85bb741d6d2e5af69114bae54c521540d5868aac476a8d9fbd1899f0a63`.
- Addresses below are module-relative RVAs. SDK declarations are under `/home/null234/Downloads/prey/Common/Prey/GameDll/`. Native evidence was read directly with `objdump -d -M intel`; no existing pickup-specific decompilation under `/home/null234/Downloads/prey/CoopPrototype/RE` was relied upon.
- All co-op source line numbers below refer to the exact PR60 commit, not a dirty working tree or a later candidate.

## Proven Native Paths

### False Can Mean Overflow UI Is Still Active

`CArkItem::PickUp` (`arkitem.h:250`, RVA `0x10B5A50`) has a local-player branch for picker `0x7777`. When the applicable item/collection checks select overflow, it builds an item-ID vector and calls `ArkPDAComponent::OpenOverflowInventory` at RVA `0x10B5B01` (callee RVA `0x160D890`). It then explicitly returns `AL=0` at RVA `0x10B5B5E`.

`OpenOverflowInventory` (`ark/ui/ArkPDAComponent.h:151`) clears its auxiliary inventory, adds the staged item through `IArkInventory::AddItem` (virtual offset `0x58`, call at RVA `0x160D919`), and opens `CArkExternalInventoryUI` at RVA `0x160D9AC` (callee `0x15EF290`). Thus `PickUp == false` does not establish that the native interaction has ended.

### Accepting Later Does Not Reenter PickUp

The overflow transfer path is:

1. `CArkExternalInventoryUI::TransferItem`, RVA `0x15EFEF0` (`ark/ui/arkexternalinventoryui.h:111`), calls the inventory UI transfer core at RVA `0x15EFF5F` (callee `0x15FF010`).
2. The core resolves the item, checks destination `CanPackItem` (virtual offset `0x78`), then calls item virtual offset `0x1E8` at RVA `0x15FF0BE`.
3. The item's actual vtable maps `0x1E8` to `CArkItem::GiveOwner(IArkInventory*)`, RVA `0x10B31C0` (`arkitem.h:249`). Its non-stack transfer calls `TryGiveInventory` at RVA `0x10B3298` (callee `0x10B6DB0`, `arkitem.h:326`).
4. `TryGiveInventory` calls destination `AddItem` at RVA `0x10B6DE7` (virtual offset `0x58`).
5. After successful `GiveOwner`, the UI core calls `OnTransferItem` at RVA `0x15FF0D8` and removes the item from the source auxiliary inventory at RVA `0x15FF0E4`.

This path does not call `CArkItem::PickUp`. The interface slot names are independently declared in `ark/iface/IArkInventory.h`.

### Closing Without Taking It Runs Native Drop

`CArkExternalInventoryUI::Close` calls `ArkInventory::OnExternalInventoryUIClose` at RVA `0x15EEA4C` (callee `0x10AC6C0`, `ArkInventory.h:237`). That function checks inventory type `6` (overflow) at RVA `0x10AC735`, enumerates the remaining items, reads their count at `0x10AC782`, and invokes their native `Drop` at `0x10AC790` (item virtual offset `0xE0`; `arkitem.h:251`, RVA `0x10B0D40`).

## Exact PR60 Mismatch

- `CoopSharedDropSync.cpp:1058-1065` reduces the granted native call to a bool. Lines `1094-1095` immediately send `PickupResult(false)` for the overflow return described above.
- The host treats that result as final rejection and releases the reservation at `CoopSharedDropSync.cpp:980-984`. This can occur while the receiver still has the native overflow UI and its staged item.
- `ModMain.cpp:19542-19562` only calls `OnNativeSharedItemPicked` and immediate pickup recovery capture from the `PickUp` hook. Deferred overflow acceptance bypasses this hook.
- Existing `GiveOwnerInventory` and `TryGiveInventory` hooks (`ModMain.cpp:19455-19475`) forward to `OnArkItemOwnerMutationHook`. That callback marks inventory dirty and logs (`CoopNativeInventoryBridge.cpp:1571-1585`); it does not complete the shared-drop transaction.
- Existing `AddItem` hook (`ModMain.cpp:19160-19166`) forwards to `OnArkInventoryMutationHook`, which likewise dirties/logs only (`CoopNativeInventoryBridge.cpp:1533-1548`).
- Existing transfer hook (`ModMain.cpp:19237-19245`) forwards to `OnSharedStorageTransfer`. That callback handles an active shared-storage lease/snapshot only (`CoopSharedStorageSync.cpp:431-456`), not a pending shared-drop pickup.
- #59's native `Drop` membership check (`ModMain.cpp:19514-19539`) correctly avoids publishing overflow rollback as a new player-authored drop. It does not preserve a PR60 reservation across overflow acceptance/cancellation.

This is a lifecycle defect distinct from releasing an unknown successful pickup after disconnect: it can start while every peer remains connected.

## Required Native Test and Limits

The native paths above are proven by local disassembly. The resulting multiplayer duplication, deletion, stale UI, or item lifetime behavior has **not** been observed by this audit and must not be labelled reproduced or fixed on this evidence alone. The Windows module hash also needs to match before treating these addresses as runtime-build evidence.

Use an isolated receiver with a demonstrably full grid, a target weapon absent from that inventory, and a real shared world drop. Preserve inventory dimensions/cell coverage and the exact stable-drop/item IDs before the interaction. A full-looking inventory or a stackable target is insufficient.

1. Receiver A uses the native pickup interaction and visibly enters overflow. Capture the false return, staged item/source inventory and shared transaction. Do not cancel yet.
2. Let the host process PR60's failure/release. Record the same lifetime being eligible again.
3. Host or receiver B attempts that same drop while A's overflow remains active.
4. A frees enough cells and accepts the staged item through the native UI. Record `GiveOwner`/`OnTransferItem`, inventories and world entities on every peer.
5. Repeat with immediate cancel, cancel after making room, repeated input, and disconnect with overflow open. Check conservation and native cleanup, not only final packet flags.

The existing full-grid runner's false return plus `owner=0` check is not proof that this staged overflow path was exercised. A future correction needs a real pending-overflow outcome resolved by native transfer/cancel, not unconditional release on bool false. This document does not claim a crash-safe receipt architecture or prescribe one from RE alone.

To reproduce a native address check without generating artifacts:

```sh
objdump -d -M intel --start-address=0x1810b5a50 --stop-address=0x1810b5dd9 /home/null234/Downloads/prey/_ghidra-input/PreyDll.dll
```
