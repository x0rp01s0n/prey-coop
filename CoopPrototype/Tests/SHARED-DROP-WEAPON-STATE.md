# Shared-Drop Weapon State

2026-11-14 follow-up to PR60, based on `f03fb4d`, protocol 259.

## Change

A newly spawned network copy previously retained the weapon archetype but not
the dropped instance's upgrades, condition or loaded ammunition. Shared-drop
packets and durable delivery receipts now carry that state. Native upgrade
levels and their actual stat modifiers are restored before pickup; recording
upgrade IDs alone does not rebuild the native stats. Sidecar restoration uses
the same helper. Old receipts without weapon metadata remain readable.

An entity already restored by the native save is only rebound to its shared
identity. Its count, weapon fields, transform, visibility and physics are not
reinitialized from the network catalog. Pending escrow remains hidden. This is
not a second inventory journal.

## Native Verification

Two isolated Windows instances used the same V51 diagnostic DLL. Tests read
fresh native inventories, grid cells, weapon fields and computed native stats,
then compare the canonical, Host-live and frozen checkpoint files. Process
identity and DLL hashes are pinned across every intentional hard kill. No Linux
game instance was used.

The fixture contains an upgraded golden pistol, GLOO gun and shotgun, with
different conditions and loaded magazines, plus pistol/GLOO/shotgun ammunition
stacks. Native ammo quantity includes loaded rounds; it is not an additional
reserve to add to the magazine. Both fields are compared independently. Initial
weapons travel through real shared-drop delivery.

| Boundary | Required and observed outcome |
| --- | --- |
| Client killed after normal Host StoredAck | Rejoin restores current inventory, upgrades, computed stats, condition, magazines, ammunition stacks and grid positions. |
| Client killed before Host storage succeeds | A real Windows file lease keeps Host inventory stale. The newer local journal restores the exact inventory and ordinary upload commits it afterward. |
| Host saves while that Client is absent | The continuing timeline accepts the newer journal despite a descendant save key; the frozen absent-Client checkpoint remains stale. |
| Explicit load of the older native checkpoint | A new timeline restores the checkpoint inventory, not the newer journal. Three separated native samples reject delayed replay. |
| Host process killed with newer unsaved Client recovery | Fresh Host loads the exact saved native file. Client receives its frozen inventory, not the later unsaved journal. Three native samples remain stable. |
| Optional journal temporary file held exclusively | An actual journal write failure does not block canonical capture, ordinary upload or Host ACK. After Client restart, the Host's committed inventory restores correctly. |
| Upgraded Q-Beam committed by Host, Commit held before Client grant, Client killed | The durable entitlement reconstructs exactly one weapon with the original upgrades, computed stats, condition and magazine. Unrelated inventory/ammunition stays unchanged; three subsequent samples reject regrant. Reconnect required retries. |
| Upgraded stungun granted, Client result held, Client killed | Reconnection to the unchanged Host repeatedly timed out. Restarting only Host networking, without native world reload or timeline change, restored exactly one stungun with matching upgrades, stats, condition, magazine and unrelated inventory/ammunition. Three samples and a normal Host upload stayed exact. This continuation checks recovery, not seamless rejoin. |

The original restart verifier incorrectly expected the silent Client's transport
entry to disappear within 90 seconds; production retains it longer. Verification
continued against the same surviving Host. Several reconnect cases initially
received no Host response and required another Connect from the same fresh
Client process. The stungun case still timed out after repeated attempts using
both loopback and the machine's LAN address. A pure Host network stop/start
allowed the same Client to connect on its first subsequent attempt, with the
same timeline, save key and durable entitlement. No native world reload or
inventory/profile file replacement was used. These initial failures and
continuations are retained; they are not flawless first-attempt reconnects.

**Readiness caveat:** inventory recovery is verified separately from this
unresolved reconnect failure. These results are not unconditional merge approval
for the complete crash/rejoin workflow. Its cause has not been isolated; the
recorded `WSA_10054` / `WSA_10022` statuses alone do not establish causality.

The harness also needed corrections: its retry report serialized PowerShell's
decorated `Get-Content` string instead of plain text, and a dot-sourced parameter
restricted the wrapper's phase variable. Neither changes production behavior.
The stungun comparison initially rejected two additional native stat-map keys
created during native initialization: `StunGunChargeTime` and
`StunGunChargeDrainRate`. All original stat entries matched. The continuation
accepts only those two extra keys at unchanged native defaults (1.5 and 1.0,
with base, actual and independently computed expected values equal); arbitrary
extra, missing or changed stat values still fail. No production fields were
changed for this verifier correction.

### Ammo-Loot Flag

Legacy inventory sidecars do not store the ground weapon's one-time ammo-loot
flag. A restored owned weapon showed a different flag while its magazine and
ammunition counts were correct. A separate native drop/pickup finalized that flag
without adding ammunition or changing upgrades/stats. Inventory assertions
therefore compare actual ammo rather than treating that world-only flag as a
restore failure. Newly spawned ground copies do preserve the source flag.

### Other Weapon Checks

The preceding V49 run covered 17 transfer variants and two return transfers:
pistol/golden pistol, shotgun, GLOO, Q-Beam, stungun and wrench; both directions;
empty/full magazines, absent reserves, condition extremes and native ammo-loot
finalization. Source and received native stats matched. Native full-grid
cancel/accept/repeat and partial-stack remainder tests also passed.

A normally network-created upgraded golden pistol survived native Host
save/load as a public source on both peers and could be picked up once. The
separate original-weapon ground test initially dropped from inside collision:
native relocation removed it before saving. Repeating from a clear position
retained the same native ground entity and fields. That was not quarantine or
deduplication and is not counted as a production fix.

## Build And Limits

- V51 native diagnostic DLL SHA-256:
  `3B3387E2BCFA67402BB91DFC64AB576567B898F8E136E4A129DECA7DAC63FEBB`.
- Normal Release build, internal controls disabled, SHA-256:
  `826BB7E07594F079D84170065E052EDBEF4365B9B04AD129AD637D703FCD644F`.
  All 123 production source files were verified against the worktree manifest
  before compilation. The normal build was compiled, not substituted into the
  diagnostic native tests.
- The selected-production/filesystem suites pass 645 distinct checks; their
  scope and faked dependencies are listed in
  [Player Checkpoints And Recovery](PLAYER-CHECKPOINT-RECOVERY.md).

Private native evidence is retained on disk as
`weapon-recovery-evidence-20261114-181612.zip`, SHA-256
`245E81294AC25C6DE4D7DA5DD92567B55907F565AE155091046130BF9B7A0F50`.
`weapon-recovery-state.json` links each phase to its completed run and any
interrupted predecessor. `recovery-runs/`, `orphan-continuation/` and
`fresh-command-replies/` retain both successful assertions and failed attempts.
`20261114-181150-459-result-held-crash-retry/rehost-isolation.json` records the
network-only restart; `20261114-181500-367-result-held-crash-retry/` contains its
final native verification. The archive also contains prior transfer/overflow
evidence, the harness, source/build manifests and binary hashes, but no native
save assets, dumps or release binaries. ZIP integrity and hash were verified
after copying it off the Windows machine.

This does not reproduce or fix the reported recycler-grenade crash, establish
coverage of every archetype, or claim that weapon equip/fire behavior was tested
in every recovery case. No additional journal schema or synthetic native-save
rewrite was introduced to make the tests pass.
