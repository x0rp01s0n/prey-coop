# Shared Drop Checkpoint Identity

The V24 negative controls proved two failures after a hard Host stop: remote
sources were deleted at Host startup, while native-origin sources became
independent per-player loot and granted twice. Inventory checkpoint selection
was correct in both cases.

## Candidate

- Mark both native and materialized public sources with the same stable name.
  The native world save already preserves that name; no second journal exists.
- After a Host world is ready, adopt its loaded visible, noninventory marked
  items once. Do not respawn them or import newer inventory recovery data.
- At ClientWorldReady, replay live public sources in the reliable stream.
  Materialization adopts a matching native source and rejects inventory/count/
  archetype conflicts rather than overwriting it.
- End that replay explicitly, pruning only unbound marked snapshot copies.
  This handles a consumed source still present in a native snapshot fallback.
  Inventory-owned items and ordinary map loot are excluded.
- Fence marked-but-unbound native pickups during replay. Offline vanilla
  pickup is unchanged. New lifetime IDs also include the Host-session nonce.

Protocol 257 adds the replay-end command. All peers need the matching build.
No PlayerStateStore or inventory-journal behavior changed in this candidate.
Recovery scans once per reset, not once per frame.

## Limits

An old native-origin drop saved without a public marker is indistinguishable
from ordinary vanilla loot. It is not retroactively converted to shared loot.
Existing network-named public sources can be adopted. Committed private
delivery recovery, native writer overlap and arbitrary save corruption remain
separate boundaries.

## Verification

The selected-production transaction suite passes 53 controls, including the
unbound pickup fence and rejection of stale/Client-origin replay completion.
Its native-world adoption/pruning dependencies are fakes, not gameplay proof.
The unchanged recovery selectors pass 15 and 36 controls respectively.
Native Windows crash/rejoin and rollback verification passed with V28 below.

V26 retained and rebound the Host source but failed the Client binding oracle:
the initial replay was gated before aggregate session readiness. V27 targets
the reporting WorldReady peer directly and permits only validated Host
Spawn/SnapshotEnd messages through the pre-gameplay receive gate. Native pickup
commands still require the normal gameplay gate. This is an event-order fix,
not a timed retry or an inventory recovery override.

V27 then exposed the same pre-session retirement in the reliable envelope,
before payload dispatch (Host sent two replay packets, Client handler received
zero). V28 uses the same narrow Spawn/SnapshotEnd classifier at both gates.
Load-control pumping, stale-world/area rejection and Host/source/context
validation remain intact; no pickup command gets this exception. The additional
classifier control checks valid replay, every transaction command, null/short
payloads and unrelated packet types.

## Native Windows Results (2026-11-12)

V28 DLL SHA256:
`5b1b9590f071d37e5af66bda686278bd74b589a70baebd94c5656f0515bffd08`
The deployed DLL and ten relevant source files were pinned and verified.

Each case used a real native drop, a native save, an exact pinned Host hard
stop, a fresh Off process loading that exact save, then a normal Client rejoin.
The oracle checked native inventories and cell layouts, live/frozen Host
records, native world entities and live shared-drop bindings. It also checked
that the saved files did not change and delayed recovery did not add items.

| Case | Restored Client | Public Source | After Repeated Native Pickup |
| --- | --- | --- | --- |
| Saved Client drop, Host crash/rejoin | 19 | 1 bound on both peers | 20, source gone on both |
| Saved Host drop, Host crash/rejoin | 20 | 1 bound on both peers | 21, Host stays 0, source gone on both |
| Save 20 before later drop, then rollback | 20 | 0 | Not applicable |
| Save 19 plus public drop, later pickup/upload 20, then rollback | 19 | 1 bound on both peers | 20, source gone on both |

Both saved-public cases then rejoined the Client again without reloading the
Host: the consumed source stayed absent and inventory counts stayed unchanged.
All four cases and both additional rejoins passed. The harness did not register
or repair saved-source bindings; only production recovery/replay could do that.

The eight focused automated suites also passed 524 unique controls covering
checkpoint storage, timeline barriers, native-save resolution, recovery
selection, receipts, escrow and delivery. Engine/persistence fakes in the
transaction/delivery suites remain explicitly distinguished from native proof.

Evidence: `checkpoint-public-v28-20261112-evidence.zip`,
`public-v28-20261112-verified.json` and `v28-source-proof.json` under the external
checkpoint-live artifacts directory. These results cover the stated boundary,
not arbitrary save corruption, legacy unmarked drops or every native-writer
overlap timing.
