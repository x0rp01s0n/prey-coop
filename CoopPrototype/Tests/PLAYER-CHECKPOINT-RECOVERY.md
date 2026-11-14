# Player Checkpoints And Recovery

Local follow-up to PR60. Current candidate protocol 258. Not pushed or merged.

The [2026-11-14 native readiness audit](SHARED-DROP-AREA-READINESS.md) keeps this
candidate on hold pending the final combined build: V32 now passes the native
solo redrop and split-area pickup counterexamples, but later saved-source and
continuing-rehost changes still need native verification on current main.
Earlier successful tests below are pinned to their stated candidate.

## Contract

The Host's live account files accept normal inventory uploads. After a native
Host save returns successfully and its final writer completes, MainUpdate freezes
a separate generation of accepted files, bound to the native `Save.CSF` contents.
Later uploads cannot modify that checkpoint. This is the Host-known inventory
when completion is processed, not a distributed barrier that waits for every
Client's unsent mutation. Deferred save acceptance alone never publishes it.

A Client restart against the surviving Host resumes the live inventory. A native
Host load restores the frozen account set and rotates a durable timeline ID,
including a cold process loading its latest save or the same slot again. A
same-world network stop/rehost preserves live state. This distinction is an
explicit loaded-world lifecycle, never inferred from equal save keys or hashes.
Stopping networking retains that known Host world and its pending native save.
Offline forward saves use the same final-writer checkpoint path. A cold offline
load instead initializes only the local Host account's checkpoint bound to the
exact loaded native file; it does not adopt newer live or Client recovery files.
Network Clients and pending automatic Client joins do not initialize that Store.
Pending or failed native loads cannot start hosting; attempting a Client join
does not clear that fence. Ordinary area transitions retain continuity.
Old-lineage packets, journals
and canonical recovery files cannot override it. Loading a checkpoint predating
this mechanism does not manufacture historical inventory from newer live files.

The journal is optional crash recovery. A valid canonical capture can still use
normal upload and native pickup when the backup write fails. Canonical capture
failure before native grant blocks that grant; a known completed grant is retried
without granting again. Inventory and delivery receipts are selected together.

V19 protects canonical-only recovery before reconnect: an already-off teardown
does not capture, and offline sidecar saves refuse to overwrite a locally owned,
integrity-checked scoped canonical snapshot. This covers native save/load
callbacks as well as joining from a loaded single-player game. Unscoped or legacy
offline inventory does not become eligible for co-op recovery.

Recovery continuity is the verified account, Host and nonzero durable timeline,
not a campaign GUID inferred from the save label. Forward native saves can use
custom slot names or truncated labels. A valid scoped snapshot is rebound to the
advertised key without changing its inventory, receipts or revision. Explicit
loads still rotate the timeline and reject that previous snapshot.

While a Client awaits the authoritative rollback inventory, neither dirty
marking nor canonical capture can relabel the old world's inventory with the new
timeline. Transport-only resets retain that barrier. Successful authoritative
apply or a full connection-identity reset clears it.

A successful native save whose co-op checkpoint cannot be captured stops the
session with explicit feedback instead of advertising a valid checkpoint. A native
save request returning failure leaves the existing session/checkpoint unchanged.
After an accepted forced save, final-writer failure or completion timeout stops
the session rather than continuing with an unpaired native/co-op checkpoint.
Failed or merely queued requests by an active Host also retain the last paired
native path used for rehosting; only final checkpoint capture advances it.
Both hosting and joining are refused while that Host checkpoint is pending,
including after networking has stopped. A failed final write or publication
invalidates continuity until a successful native load.

## Automated Checks

V28 reran 524 checks across the following suites. The separate SessionHello
freshness-policy suite was previously tested. The delivery shell runner repeats the 15 snapshot
selection checks; those are counted only once here.

| Checks | Coverage |
| --- | --- |
| 53 | Transaction contention, cancellation, disconnects, duplicate/reordered/lost packets, unbound public pickups and replay completion/classification |
| 33 | Native-delivery dependency controls, overflow callbacks and optional journal failures |
| 52 | Receipt parsing, validation and upload merge |
| 15 | Whole inventory/receipt snapshot selection |
| 36 | Canonical/journal selection, forward-save rebinding, ACK retirement, revision floors and offline preservation |
| 150 | Production checkpoint Store and metadata against filesystem fixtures |
| 163 | Selected-production save completion, explicit loaded-world lifecycle, offline save/load continuity, timeline barriers and reliable world-change handling |
| 22 | Native-save path resolver against filesystem fixtures |

The filesystem suite executes the production Store with real temporary files:
live versus frozen state, same-slot replacement, continuing saves, restart,
rollback, missing/corrupt checkpoints, account-set replacement, interrupted
staging, write failures, aliases and symlinks. Full inventory/receipt/layout bytes
are compared, not only item counts. It also exercises the actual strict snapshot
revision parser. 150 checks passed.

The selected-production barrier suite executes the pending-save callbacks and
dirty/capture/reset controls with engine and network dependencies faked. It
checks both callback orders, deferred replay, matching completion, timeout,
overlap, fail-stop behavior, failed-save path retention and rollback fencing.
It also executes the Host's world-change branch, stale-world reliable retirement
and actual peer-removal transport cleanup. A warm join retains already-ACKed
sequence frontiers; a true runtime restart still clears the old endpoint. It
executes the production timeline open/finish policy and native load-request hook,
including cold same-key restore versus same-world rehost, failed/incomplete
loads, Client-role changes, safe-empty checkpoints and genuine store failures.
Offline cases include pending writes across teardown, both role-entry gates,
failure fencing, and a cold load's captured target remaining authoritative even
if incidental save callbacks change the general last-path field. A native
Client or pending automatic Client join cannot bootstrap a local Host Store.
163 passed.

The actual native-save resolver also runs against temporary filesystem fixtures.
22 checks cover exact final-writer user aliases, campaign identity, missing-file
failure without campaign fallback, and existing path formats.

The selected-production and delivery suites fake their native engine/transport
dependencies. The counts above are not native gameplay proof.

## Native Windows Checks

The final V21 verification DLL was built on Windows, with diagnostic-only native
UI controls enabled. Its SHA-256 is
`3BA7E4F1C134EEE4A4ACBCBEC26FCF7A0434BBC034771AE39BD4A3B119B62511`.
Both instances used that DLL. Each step checks exact process identity, fresh
extractor replies, native inventory exports and on-disk account/checkpoint
hashes. No Linux game instance was started.

The ordered 13-step matrix and two additional canonical-write fault cases passed:

| Native case | Observed result |
| --- | --- |
| Same-slot save, later uploads, hard Client restart | Frozen inventory stayed at 1 while live inventory advanced to 3; reconnect restored 3. |
| Explicit reload of that same native checkpoint | Timeline and epoch changed; actual Client inventory returned to 1 and did not recover the later 3. |
| Optional journal file locked | Canonical capture, ordinary upload and Host StoredAck still completed exactly once. |
| Outbound drop, Host upload blocked, journal blocked, Client killed | Canonical captured 19 before publication; Host retained 20. The Client was killed 439 ms after the native drop. |
| Host saved while Client was absent, then cold single-player load and rejoin | Client and Host live inventory converged to 19; world drop stayed at 1; the absent-Client checkpoint stayed at 20. Full recovered inventory/layout/receipt payloads matched, excluding only integrity and the legitimate save-label rebind. |
| Host receipt temporary-file open and final-file replacement denied | No native grant; the source remained public. Releasing each real Windows file lease and retrying the same lifetime delivered exactly 1. |
| Full native grid and overflow menu cancellation | Cancel granted nothing; freeing exactly two cells and transferring through the native UI granted once; repeated transfer was rejected. |
| Stack at 999 receiving 2 | First native grant accepted 1; cancelling retained the private remainder without reopening. Freeing cells delivered the remaining 1; repeat pickup did not duplicate it. |
| Deferred native QuickSave | Actual `force=0` acceptance, `force=1` replay and matching final write preceded checkpoint publication. The changed native file fingerprint and frozen/live inventory of 1001 matched. |
| Client canonical temporary-file open and final-file replacement denied | Precommit grant was refused. Within 4 seconds, both replicas showed the same visible public source with count 1 and no winner. Durable Client/Host bytes stayed unchanged. Unlock and retry delivered exactly 1 with matching receipts in all three stores. |

The full evidence is archived outside the repository as
`checkpoint-v21-evidence.zip`. Entry points are `v21-matrix-result.json`,
`v21-final-verification.json`, `v21-source-proof.json` and
`v21-canonical-fault-evidence/`. Earlier failed observations and their subsequent
guarded continuations are retained, not relabelled as passing runs. Native UI
cleanup used the existing vanilla cancel path, not direct input-state writes.
Original profiles and mod directories are backed up and restored with complete
file/directory/hash manifests; restoration evidence is retained separately.

### V22 Session Lifecycle

The V22 follow-up DLL has SHA-256
`2644E6963AAECB53B807742FBAF3A3F71AEDEB6AAD8FCED0BA6D513D4A6088F1`.
Both Windows instances used that exact DLL. The same saved checkpoint contained
Client count 1; subsequent ordinary uploads advanced live state to 3.

- Stopping and restarting only Host networking retained the timeline and restored
  live count 3, without replacing the frozen count 1.
- Killing the Host process, restarting it and natively loading that exact same
  save rotated the timeline. The surviving Client received count 1, not its
  newer count 3. Host native inventory returned to its saved 40-item layout;
  all archetypes, counts, grid dimensions and positions matched, excluding
  runtime entity IDs.
- After that authoritative apply, the old canonical file could remain on disk
  under its old timeline. It was ineligible for recovery. Stable native samples
  remained at 1; a normal upload then captured canonical count 1 under the new
  timeline and received a fresh Host StoredAck. Frozen bytes remained unchanged.
- A separate fresh Client process against the surviving Host recovered live
  count 3 on the same timeline. Loading its exact saved checkpoint through the
  native Host load path then rotated timeline and epoch, restored count 1 and
  rejected the later recovery state. The Host retained its native saved count 1.

V22 evidence is archived as `checkpoint-v22-evidence.zip`. The cold-Host entry
point is `v22-cold-host-native/state.json`, whose
completed phase is `20261110-224416-619-after-restart/result.json`. The initial
fixture used debug-created `NO_SAVE` Host items and was rejected, not counted
as a product failure or passing proof. The corrected fixture retained real
native-save inventory. An earlier verifier also expected canonical capture too
soon after authoritative apply; its failure is retained beside the completed run.
The Client restart/rollback entry point is `v22-client-regression/state.json`,
with the final wrapper result in
`20261110-225027-104-wrapper-rollback/result.json`.
The V22 Host comparison does not assert weapon condition, mods or ammo; Client
checks cover the controlled item count and lineage rather than full receipt
payload equality. The broader V21 evidence above remains separately identified.

### V24 Offline Save And Rehost

The V24 Windows DLL has SHA-256
`88AAE0699E29F94170AF93411F869AA5592A3763BAF1F62B7BD00044526C3608`.
Both instances were launched with this build. All four native boundaries passed:

| Native case | Observed result |
| --- | --- |
| Stop networking, save to a new slot, rehost | Same world and timeline; native Client, Host live file and frozen B all retained count 3. The original frozen A generation stayed at 1. |
| Stop networking, overwrite the same slot, rehost | Same preservation result; the exact native slot changed bytes and its replacement checkpoint contained 3. |
| Fresh Host process, load A while Off, save a new B, host/rejoin | The cold load established a new timeline from frozen A1, not newer live/canonical3. Actual Client inventory changed from 3 to 1; live and frozen B also contained 1. |
| Fresh Host process, load A while Off, overwrite A, host/rejoin | The same cold restoration held for same-slot replacement; the loaded-A timeline survived the offline save and first hosting. |

The tests compare exact native save hashes/fingerprints and four stable exclusive
file samples. Before any explicit Client upload, they check runtime/durable
timeline and save binding, actual native inventory and Host/Client grid shapes.
Later normal uploads receive Host StoredAck on the correct timeline; ten native
samples reject stale inventory replay. Host native inventory/layout and its
native-load counter remain unchanged across saving/rehosting. Item condition,
mods and ammo are not part of the shape comparison.

The first cold new-slot save returned 0 before serialization and is retained as
`TEST_ERROR`; the command runner waited for the successful action until timeout.
A delayed retry in the same process, without profile/checkpoint edits, returned
1, received its matching native writer callback and passed the restoration
oracle. The exact native eligibility gate behind that initial rejection is not
proved. The other three cases completed without that retry.

Negative controls are retained: V22 lost Client inventory in both warm offline
save variants; V23 passed warm new-slot rehosting but lost inventory after a cold
offline load/save. V24 fixes the shared lifecycle/checkpoint path rather than
adding an inventory restoration workaround to the harness.

Evidence: `checkpoint-offline-v24-20261111-evidence.zip`, entry point
`offline-final-v24-20261111/final-verification.json`. Each case points to its
completed native oracle and the earlier nonpassing attempt remains listed.
`v24-source-proof.json` binds the tested DLL to ten production source files.
Earlier failures remain in the separately hashed V22/V23 evidence archives.

### V24 Host Crash With Public Drops

The next native Windows round found two release-blocking shared-world failures
on the same DLL, despite correct frozen Client inventories and grid layouts:

- Client20 drops1, accepted inventory19/public1, save, hard-stop Host, load exact
  save and rejoin: native load contains the drop, but starting Host deletes it.
  Client remains19 with an unbound copy; Host has no public source.
- An existing native Host item is dropped and saved. After the same crash/load
  boundary, both visible copies survive without shared bindings. Native pickup
  grants Client20 ->21, then Host0 ->1: two grants from one public quantity1.

The registry exists only in memory. Reset deletes network-named saved world
copies, clears both binding maps, and leaves native-origin copies behaving like
independent vanilla loot. This is a public-source persistence/rebind failure,
not evidence that the inventory journal should merge a newer timeline.
Preserving visuals alone cannot fix the second case. Ordinary per-player map
loot must not be converted to shared loot to address it.

Rollback-before-drop passed with native/live/frozen20 and no post-save source.
The inverse save-public-then-pickup boundary also restored saved inventory19,
not later20, but still failed its public-world oracle. Correct inventory
rollback therefore does not establish conservation across saved shared drops.

### V28 Saved Public Source Recovery

The central shared-drop fix now preserves and adopts marked native saved world
entities rather than deleting or independently respawning them. WorldReady
replays bind each loaded Client copy; reliable replay completion removes stale
unbound snapshot remnants. No additional journal was introduced and checkpoint
inventory selection is unchanged.

Both V24 failure boundaries now pass native Windows testing with a fresh Host
process loading the exact saved A: Client-origin 19/public1 restores to 19 and
one bound source; Host-origin 0/Client20/public1 restores to those exact counts.
Repeated native Client pickups deliver exactly one, and both subsequent live
rejoins keep the consumed world source absent. Rollback-before-drop restores
20/no source; rollback-before-later-pickup restores 19/public1, then pickup20.
Layouts, native entities, live/frozen records, immutable saved bytes and delayed
recovery are checked independently. See `SHARED-DROP-CHECKPOINT-IDENTITY.md` for
the pinned DLL, native proof scope and failed intermediate candidates.

## Limits

- A checkpoint cannot include an inventory mutation the Host has not received.
- Native world serialization and Client inventory capture are not one distributed
  atomic snapshot. Uploads accepted while the final native write is pending may
  enter the account checkpoint captured at completion.
- Historical saves made before frozen account snapshots may have no recoverable
  Client checkpoint. Newer journals must not fill that gap after rollback.
- Checkpoint-generation retention is not yet bounded by native save deletion.
- This does not establish recovery from arbitrary Host/world-save corruption or
  machine power loss.
- Switching into a network Client while a native save is only queued (before
  its forced replay) is not natively proven. The role-entry gate covers accepted
  forced saves, not that earlier vanilla delayed-save queue.
- V28 proves the four stated Host-crash/public-drop boundaries, not legacy
  native drops saved without public identity markers. Client offline-save then
  joining another Host and exhaustive item-detail restoration such as auxiliary
  ammo still lack native proof. The earlier offline-save/rehost boundary is
  tested; it is not a claim about every offline or crash combination.
