# Hack And Repair Sync

Windows verification on main `16becac` plus this change, protocol 260.

This follows Blurrytorch's Nexus report: hacked safes could appear open without
allowing loot access, hacked Operators stayed hostile, and repaired recyclers
or turrets only worked for the player who repaired them.

## What Changed

Safe unlocks now follow the keypad's completed native state change, not just
the opening animation. Interactive machines use the existing repair-state
persistence path. Turret repair callbacks receive the correct native listener
pointer, and a local repair is forwarded even when another player owns the turret.

For an NPC owned by another player, the hacking minigame remains local, but its
effect waits for that owner to confirm it. The native Hack function then applies
the result; faction, light and interaction flags are not patched separately.
A pending completion follows ownership changes. A rejected completion does not
leave a speculative local hack that would need a separate undo operation.

Two problems found during these tests are included: a new owner could send an
older snapshot sequence than its ownership grant, and ragdoll bit 16 did not fit
the compact packet's 16-bit locomotion field. The latter rejected the entire
snapshot. That field now retains all 32 bits, adding two bytes to the packet.
Independent client area loads also refresh the existing connection deadline,
as host-save loads already did.

## Native Tests

- Safe success and failure from each peer: lock state and actual container
  interaction eligibility matched on both instances.
- Fourteen authored repair targets, including recyclers, electrical boxes,
  grav shafts, a fabricator and operator dispensers, plus three turrets:
  checked changes in both directions and unchanged-state calls.
- Rejoin after the host changed repairs and unlocked a safe: the returning
  player received those states and a newly spawned, hacked Operator.
- Three-player hack tests: rejection, timeout, duplicate messages, ownership
  changing to a third player or to the requester, and acceptance/rejection
  between two clients in a different area from the host. All seven cases passed.
- Without a pending hack, 24 ownership changes preserved native hacked state,
  faction, relationships and light, both before and after hacking an Operator.
- Two further runs checked a genuinely available native Hack prompt before,
  after and after settling each ownership change. All 12 handoffs and 76 prompt
  observations passed, including the return direction that failed before the
  wire fix. Both peers also passed the native snapshot roundtrip selftest.

The tests read native state and actual interaction eligibility, not screenshots.
The automated interaction cases call native completion methods; they do not play
every minigame or verify every resource-spending UI. The positive-prompt fixture
uses an alive stunned Operator and normal view placement, without forcing HUD
selection. It verifies Hack availability, not a hacked Operator's service prompt.
Fresh ChargeTrap coverage was not repeated on this base.

Initial failed runs were retained, including an inconclusive recycler read and
view setups that missed the ragdolled body. The recyclers passed subsequent
repetitions; neither those setup failures nor older results were counted as new
passes. Raw native captures, process/build identities and restoration records
are retained in the local `prey-pr-artifacts-20261115/hack-repair` archive.

The portable interaction/packet regression is runnable with
`bash CoopPrototype/Tests/run_interaction_policy_tests.sh`. It includes the
ragdoll roundtrip that failed before this fix, along with pending-request,
ownership-change, stale-reply and cross-area routing checks.
