# PR60 Verification: Hold

Historical evidence for the original PR60 head. The current checkpoint/live
contract and verification scope are in [Player Checkpoints And Recovery](PLAYER-CHECKPOINT-RECOVERY.md).
Results below are retained unchanged; they do not describe the final candidate.

Tested 2026-11-09, exact open PR head
`070cb0bae35aec382acca0b6511f8642d41d5f15`, based on
`1419eaaf1fd441b37d2a995040bca21005e59c9f`. Production code is unchanged.

## Blocking Findings

### Successful Pickup Is Released After Losing Its Confirmation

Reproduced with two native Windows instances, not only engine fakes:

1. A normal Host drop and Client pickup conserved the total: Host `6 -> 5`,
   Client `0 -> 1`; both shared records became terminal at version 3.
2. For a second drop, hold Host reliable sends until the pickup request has
   reserved the item, pause Client reliable sends, then resume the Host. Incoming
   grant/native pickup still run, but the Client's result cannot reach the Host.
3. The Client count became `2`, with `live=0`, `pending=1`, `granted=1` at
   version 2. Both its account sidecar and scoped recovery journal contained
   the two-unit stack before termination. The Host had four units and one
   reserved world unit, so the total was still six.
4. Force-terminate only the recorded Client PID. After native peer removal,
   the Host record became visible/live/unreserved at version 3. A fresh native
   inventory dump proved that exact world entity was not stored in its inventory.
5. The Host could natively pick up this same stable drop again:
   `nativeSuccesses=1`, Host count `4 -> 5`, terminal version 4.
6. Start a new Client process and join directly from the menu, without loading
   its own checkpoint or replacing any journal. It restored two target units.
   The unchanged Host retained five: seven inventory units versus the original
   six. The native reconnect conservation check failed with exactly one extra
   unit. This establishes retained duplication, not just temporary re-exposure.

Stable drop: `16516639491948767672`; target archetype:
`10739735956144611856`. No synthetic inventory grant was used at either pickup.
The replacement Client had no old stable-drop registry entry; this limits loose
world-entity coverage but cannot explain away the excess already in inventories.

The transition is `ReleaseSharedDropReservationsForPeer ->
ReleaseSharedDropReservation`: an unknown outcome becomes `live=true` without
reconciling the winner's durable acquisition. A timeout cannot distinguish
"grant never applied" from "applied, confirmation lost".

### Overflow Is Not A Finished Failure

Native `PickUp` can open the organize-inventory UI and return false. Accepting
the staged item later uses `GiveOwner`/`TryGiveInventory`/`AddItem`, not a second
`PickUp`. PR60 sends final failure and releases the reservation before this
interaction has finished. Existing mutation hooks do not finish that transaction.

The exact native module hash on Windows matches the disassembly audit. This is
control-flow evidence; the multiplayer accept/cancel/competition outcome has not
yet been demonstrated in native gameplay. See `SHARED-DROP-OVERFLOW-AUDIT.md`.

## Executable Regression Checks

The suite compiles unmodified production transaction methods and record layout
against engine/transport fakes. Sixteen controls pass, covering normal success,
duplicates/retries, rejection/retry, contention, scope/source validation and the
native post-pickup apply-depth guard. Two retained counterexamples fail:

- Losing the result after successful acquisition, then removing the peer.
- After a failed winner, a third observer's first explicit pickup invocation
  only refreshes its stale version; its second invocation succeeds. This is a
  responsiveness gap, not a demonstrated physical F-key or duplication bug.

The ordinary gate exits 1. `--expect-baseline-failures` confirms exactly those
two failures and is deliberately not a fixed-build pass. Native travel/reset,
three-player input behavior and journal recovery are not modeled by this suite.

## Native Build And Evidence

- Co-op DLL SHA-256:
  `472b8e0ec98e1672ea843256639ee9eb8d4dba023ba300e4286977a25dc45d7c`
- Native PreyDll SHA-256:
  `0485c85bb741d6d2e5af69114bae54c521540d5868aac476a8d9fbd1899f0a63`
- Only disposable Windows Host/Client profiles were used; their previous build
  and full profiles were backed up before testing.
- After preserving the evidence, the previous DLLs and profiles were restored;
  both restored DLL hashes are
  `e37205c3da03850085fd226b91751e993b7f129e09661c35a7e9335baeb00cea`.
  The disposable games are stopped. Test-time profiles remain preserved at
  `C:/Users/null234/Saved Games/Arkane Studios/PreyCoop-Repro-20261103-after-pr60-20261109`.
- Local artifacts: `/home/null234/Documents/Codex/prey-pr-artifacts-20261109/`.
- Remote evidence: `D:/PreyCoop-PR60-20261109/results/`, especially
  `shared-drop-20261109-231956-091-hold_after_grant` and
  `shared-drop-20261109-232140-464-observe_after_disconnect` and
  `shared-drop-20261109-232811-012-observe_after_reconnect`.
- Second Host pickup: `D:/PreyCoop-HackRepair-20261106/results/pr60-host-pickup-after-client-crash-20261109/`.

The first native success-run attempt stopped on an invalid test assumption:
native Drop retains a nonzero owner ID. The harness was corrected to use fresh
inventory membership, not `owner==0`, for world eligibility. Its manual
continuation supplied the successful pickup control above. A short initial
disconnect observation ended before peer removal; the later observation caught
the real release. Neither harness interruption is counted as a product failure.

## Decision

Do not merge this exact PR as a crash-safe/full-inventory transaction fix.
Resolve unknown acquisitions without recreating an already granted lifetime,
track overflow until genuine native accept/cancel completion, and rerun the
native conservation checks. Simply committing or releasing every disconnected
reservation would trade duplication for item loss or stuck loot.
