# Shared-Drop Native Boundaries

Historical v10 boundary evidence. The current optional-journal/checkpoint
contract and verification scope are in [Player Checkpoints And Recovery](PLAYER-CHECKPOINT-RECOVERY.md).
Results below are retained unchanged, including the older quarantine behavior.

2026-11-10, local PR60 follow-up; not pushed or merged. Protocol 255.
Windows diagnostic v10 DLL SHA-256:
`475a998b50f3d6042e34a786ca329b358ca24222d6c99b8bad5f1874507606f8`.
Production source-overlay SHA-256:
`c3bf5794dde98a622e99a724f6a1f694b34ae3f285cf826f0d73a5ca73385737`.

## Native Results

Two disposable Windows instances used the same build. Every case pins process
identity, DLL hash, Host save/session/epoch and exact native inventory counts.
UI controls call native inventory transfer/cancellation, not a simulated grant.
They are diagnostic-only and default off; this does not test physical controller
or keyboard navigation.

| Boundary | Observed result | Result directory suffix |
| --- | --- | --- |
| Full 45-cell grid: commit, cancel, reopen, free two cells, accept | No grant on cancel; exactly one pistol on acceptance; repeat rejected | `090822-467-preflight` |
| Nearly-full banana stack: partial acceptance and cancellation | Exactly 1/2 accepted; private remainder retained; later exactly 1/2 completed | `090859-551-preflight` |
| Host file-open failure before ownership commit | No Client grant or durable assignment; public source retained; same-lifetime retry succeeds once | `090600-091-open` |
| Host atomic-replacement failure before ownership commit | Unchanged final file, valid uncommitted candidate, released public source; retry succeeds once | `091415-730-replace` |
| Client canonical-state write failure after native acceptance | Multiplayer quarantined; cold Client rejoin restores exactly one banana and 19 filler wrenches, with paired receipt and no public source | fault `090935-887-sidecar`, verify `091236-917` |
| Client recovery-journal write failure after native acceptance | Same conservation result after cold Client rejoin; canonical inventory/receipt selected together | fault `091442-782-recovery`, verify `091724-308` |

Result directories start with `shared-drop-20261110-`, `host-write-20261110-`,
`client-write-20261110-` or `verify-write-fault-recovery-20261110-` as appropriate.
Both Client restarts preserved the original Host process, world, session and save.
No Client checkpoint or journal was copied into place to make recovery pass.

The Client wrappers stopped at their process-exit check after issuing the exact
Client termination. Separate fresh processes subsequently confirmed those PIDs
absent, restarted the Client and verified recovery. These are successful native
boundary checks with manual continuation, not uninterrupted wrapper passes.
The replacement test also needed a corrected observer: duplicate Ready packets
can overwrite a single last-event string. Its fallback requires the actual file
lock, checksum-valid fresh candidate, unchanged final file and released record
together before reservation expiry. Earlier failed attempts remain in evidence.

## Defect Found

A whole-item native Drop changed inventory but did not reliably mark the local
recovery journal dirty. The first write-fault preflight caught canonical wrench
count 19 versus journal 20 before any fault was armed. The existing local Drop
hook now marks inventory dirty after native Drop and before publishing the drop.
Both v10 Client fault cases verified the corrected 19/19 baseline before testing.

## Automated Stress And Limits

All 150 selected-production dependency-fake checks pass: competing pickups,
loss/reordering/duplicates, cancellation, partial delivery, receipt-table limits,
write failures and recovery selection. Source-wiring assertions and
`git diff --check` also pass. These do not replace the native results above.

- Outbound Drop publication is still not atomic with the debounced journal.
  A source-client crash before journal capture remains an unclosed boundary.
- Host-crash world identity/deduplication and join-window public-drop replay
  remain outside these surviving-Host tests.
- Weapon-generated ammunition, equipped chipsets and actual recycler rejection
  of a private remainder are not proven by the target-item grid tests.

These results do not establish complete inventory crash safety or unconditional
merge readiness. Detailed observations, failed attempts, source overlays and
runner scripts are retained in `boundary-evidence-20261110.zip` under the local
`prey-pr-artifacts-20261110/escrow-boundaries` artifact directory.

Both disposable games were stopped. Original mods/profiles were restored;
independent verification matched both original DLL hashes and all 71 state-file
hashes against the backup. Test profiles remain preserved separately on Windows.
