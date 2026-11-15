# Shared-Drop Escrow Verification

Historical evidence, superseded for the current recovery contract by
[Player Checkpoints And Recovery](PLAYER-CHECKPOINT-RECOVERY.md). The earlier
results below are retained unchanged and are not final-candidate claims.

This file retains the earlier v2/v4 evidence. Current v10 overflow and actual
Windows storage-fault results are in `SHARED-DROP-NATIVE-BOUNDARIES.md`.

2026-11-10. Local follow-up to PR60 head
`070cb0bae35aec382acca0b6511f8642d41d5f15`; not pushed or merged.
Protocol 255. The original PR60 duplication counterexample remains documented
separately in `PR60-VERIFICATION.md`.

## Change

Prepare does not grant inventory. Ready lets the Host persist account ownership
before sending commit. The winner then uses native pickup and stores the actual
inventory delta together with its delivery receipt. Only precommit reservations
expire or cancel on disconnect; lost postcommit acknowledgements cannot recreate
world loot. Recovery restores the selected inventory and its receipts together.

Unknown native outcomes or failed receipt persistence stop multiplayer until a
world reload. Reserved/committed private sources are excluded from recycler
processing. Native full-inventory acceptance is observed through GiveOwnerInventory,
not inferred from the earlier PickUp return value.

## Native Windows Evidence

Two disposable Windows instances were used. Each held transaction records the
exact DLL hash, process IDs/creation times, Host save/area/epoch, fresh native
counts, and stable world-drop probes. Rejoin starts a new Client directly from
the main menu while the original Host remains running; it does not load a Client
checkpoint or replace its journal.

The v2 DLL, SHA-256
`b742715a1e946b17882ec6ee03bd56be9b2b8bd2a18309a18cb8489ec3a24efe`, passed:

- Normal banana pickup: Host 6 -> 5, Client 0 -> 1, terminal world record.
- Preparation timeout: no Client item; after 30 seconds the shared item returned.
  Releasing the delayed Ready queue did not commit the cancelled transaction.
- Client hard kill after durable Host commit, before commit delivery: after rejoin,
  Host 3 / Client 2 versus baseline 4 / 1; no eligible copy of that world lifetime.
- Client hard kill after native delivery/receipt, with result delivery held: after
  rejoin, Host 2 / Client 3 versus baseline 3 / 2; no second inventory grant.

The v4 DLL includes the later storage-fault fence, alternate recovery selector,
recycler exclusion, and retired-private-source recovery. It builds on Windows:
`9c85e269a0ee9dd1902f90a62017bf9ce4d948ac44aab44b946d2c2ff28c15a7`.
Source bundle SHA-256:
`568e0753f72c101722a51f94069b26079c8d30d434acfc16765e3d11a0cc4b48`.

The final v4 build passed these native checks:

- Wrench pickup: Host 2 -> 1, Client 0 -> 1; native acquisition and a persisted
  receipt were observed, with no eligible world copy of that lifetime.
- Preparation timeout: the Client received nothing; the world item returned and
  a delayed Ready did not revive the cancelled transaction.
- Hard kill after native delivery and receipt persistence, before result delivery:
  after rejoin, Host 4 / Client 4 versus baseline 5 / 3. The original Host process,
  save, epoch and area remained unchanged; the committed world copy stayed absent.
  Fresh inventory exports also retained exactly one wrench on each peer, including
  the wrench acquired by the Client before its restart.

The first v4 restart attempt timed out with `no_response_from_host`; the Host
reported `WSA_10054` after the old Client was killed. A second connection attempt
from that same restarted Client joined successfully without restarting or loading
the Host, replacing the Client journal, or resetting inventory. Its cause was not
isolated. Inventory conservation passed after that retry, not on the first attempt.
The before-delivery hard-kill boundary was native-tested on v2, not repeated on v4.

Final-build result directories under the Windows artifact root:

- `shared-drop-20261110-052827-189-preflight`: wrench pickup.
- `shared-drop-20261110-052857-135-preflight`: preparation timeout.
- `shared-drop-20261110-053143-496-preflight`: native receipt/result held.
- `restart-client-20261110-053224-584`: initial reconnect timeout.
- `shared-drop-20261110-053654-560-preflight`: successful reconnect retry.
- `shared-drop-20261110-053730-812-preflight`: post-reconnect conservation.

## Historical v4 Automated Checks

The final source passes 49 transaction controls, 26 delivery/recycler/remainder
checks, 50 metadata checks, and 15 recovery-selection checks. These compile the
selected production methods, but engine calls, transport, and persistence results
are faked. They cover packet loss/reordering/duplicates, scope rejection,
contention, pre/postcommit disconnects, partial acceptance, overflow callbacks,
receipt regression, and storage failures. They are not native UI or disk-fault proof.

## Boundaries At The v4 Checkpoint

- Real organize-inventory UI accept/cancel and partial-stack transfer still need
  native proof, as do receipt-write failure and recycler rejection of a private remainder.
- Host-crash world-drop identity/deduplication is not solved by this design. The
  native crash tests above preserve the Host session.
- A pre-existing join-window public-drop replay gap is separately described in
  `SHARED-DROP-JOIN-AUDIT.md`; no native reproduction or fix is claimed here.

Keep PR60 on hold as a complete crash-safe/full-inventory fix until those native
boundaries are checked. Artifacts live under
`/home/null234/Documents/Codex/prey-pr-artifacts-20261110/escrow/` and
`D:/PreyCoop-PR60-20261109/escrow-results/`.

After testing, both sandbox games were stopped and the pretest mods and profiles
were restored. Both restored DLLs were verified against SHA-256
`e37205c3da03850085fd226b91751e993b7f129e09661c35a7e9335baeb00cea`.
The test profiles and DLLs remain preserved in
`D:/PreyCoop-PR60-20261109/escrow-after-tests-20261110/`.
The compact evidence archive contains 1,953 files and was copied to the local
artifact directory as `escrow-evidence-20261110.zip`, then checked for archive
integrity and SHA-256
`32c1b166f1cb7dc1842a36f546c5204a37ab6842e4a63ea32d00e54805579ed0`.
Its manifest identifies omitted large files, which remain on the Windows drive.
