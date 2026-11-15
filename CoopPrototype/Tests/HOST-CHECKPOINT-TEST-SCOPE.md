# Host Checkpoint Test Scope

`run_host_player_checkpoint_tests.py` compiles the production filesystem store
and exact recovery metadata parser. It exercises full inventory/layout/receipt
bytes, immutable checkpoints, forward saves, rollback, same-slot overwrite,
missing/corrupt files, write failures, symlinks, lineage and revision floors.
Integrity validation is a fixture dependency, not the game's hash implementation.
Current run: 150 filesystem checks passed.

`run_host_timeline_barrier_tests.py` executes the production pending-save methods,
native-save-return callback and dirty marker, plus exact selected
capture/reset/packet-lineage guards. Engine, network and persistence dependencies
are faked. It checks both return/write-completion orders, deferred requests,
exact native campaign/slot matching, failure-only timeout, overlap, reset and
one-time publication. It also checks
the rollback capture fence and rejection of stale authoritative Start packets;
it does not prove a native game save or load. The packet observation wrapper
maps the original void early return to false without changing its condition.
Current run: 163 timeline/pending-write/transport checks passed; production hook/reset/tick
wiring is checked separately by the runner.
These include failed and deferred Host requests retaining the prior completed
native path, so rehosting cannot combine save A's key with failed save B's file.
The selected Host Hello world-change branch preserves the reliable sequence,
reorder and ACK frontiers. Actual stale-world retirement still advances the
stream without applying its payload, and actual peer removal resets only that
endpoint. This prevents a loaded-singleplayer join from waiting forever for
packets already ACKed before the world epoch changed.

The same runner executes the production timeline open/finish methods and native
load-request hook. Exact Host-start gates and Client-role transitions are also
extracted. Cold process/native loads rotate even with identical save identity;
network-only rehosting preserves live state. Loading and failed-load fences
survive Client entry and network teardown. A successful native completion, not
an accepted initial load request, establishes the next usable world. Safe-empty
historical checkpoints are distinguished from store I/O failures. These checks
fake the engine and store; separate Windows proof is required for the lifecycle.

The offline controls execute the actual Host/Off eligibility helper, native
callbacks and Store-opening policy. Pending checkpoint work survives a known
Host world's network teardown and blocks both hosting and joining until its
completion. Failed publication invalidates continuity. A successful cold offline
load initializes only its captured native target, not an incidental later
save-path value; Client and pending-autoclient loads are excluded. The engine
and Store dependencies here remain fakes, so these controls alone do not prove
the native offline save/rehost boundary.

The final V22 Windows DLL was also exercised through those native boundaries.
Network-only rehosting retained the live timeline/inventory. Killing the Host
and loading the exact same save in a fresh process instead restored the frozen
Client checkpoint and the Host's native 40-item count/layout snapshot. A hard
Client restart against the surviving Host retained newer live inventory; a
subsequent active-Host native rollback restored the older checkpoint and rejected
later inventory from the previous timeline. See the V22 evidence and limits in
`PLAYER-CHECKPOINT-RECOVERY.md`; these are not extrapolated from the faked checks.

V24 additionally passed offline native save/rehost to new and overwritten slots,
both in a continuing Host process and after a fresh Host loaded the exact older
save while Off. Continuing worlds preserved live count 3; cold worlds restored
frozen count 1 instead of replaying live/canonical3. The oracles check actual
native inventory, save bytes, lineage, full grid shapes and later ordinary
StoredAck. One initially rejected native save remains TEST_ERROR beside its
successful delayed retry. See the V24 evidence section for provenance and limits.

The subsequent V24 native Host-crash/public-drop tests fail outside those
inventory-only guarantees: Client-origin saved sources are deleted at Host
startup, while native Host-origin sources lose their shared bindings and can
grant once to each peer. Both cases retain correct frozen inventory/layout.
See `PLAYER-CHECKPOINT-RECOVERY.md`; the automated passes do not cover or negate
these observed public-world failures.

V28 fixes and natively retests that separate boundary: loaded public sources
retain stable names and are adopted/rebound, then replayed to the joining peer.
Both origin cases now grant exactly once after a hard Host stop and exact saved
reload. Both consumed-source live rejoins and the drop-after-save / pickup-after-
save rollback cases also pass. See `SHARED-DROP-CHECKPOINT-IDENTITY.md` for exact
counts, the two failed intermediate candidates and remaining limits. This does
not replace or reinterpret the V24 negative controls above.

`run_native_save_resolver_tests.py` executes the unchanged production resolver
and candidate helpers against real temporary save files. Its 22 filesystem
checks cover exact `%USER%/SaveGames/` expansion, mixed-case prefix and Windows
separators, explicit campaign selection, a literal-alias shadow, no fallback for
a missing exact alias, malformed suffixes and existing absolute/bare-slot paths.
Only the effective profile root, active campaign and readable-file dependency
are supplied by the fixture. The Linux path adapter translates Windows
separators; suffix case matches the real filenames, rather than pretending Linux
provides Windows' case-insensitive filesystem behavior.

## Deferred Native Save Evidence

The local Prey binary disassembly establishes that `SaveGame` returning true
does not by itself mean a deferred save has executed:

- `CCryAction::SaveGame` is RVA `0x5C7DD0`. At `0x5C7E01` it retains
  `forceImmediate` from `r9b`. At `0x5C7F99` the result starts as true; a false
  immediate flag branches at `0x5C7F9F` to `0x5C8187`.
- That branch stores delayed save method/reason/countdown and path, then returns
  true at `0x5C8244`. It does not execute the serializer call at `0x5C80AD`.
- `CCryAction::PostUpdate` is RVA `0x5C69D0`. At `0x5C71A7` it drains the
  countdown; `0x5C71D7` clears the queued method; `0x5C71E1` sets `r9b=1`.
  The virtual call at `0x5C7206` replays `SaveGame` with the stored path/reason
  and `forceImmediate=true`.

Source evidence: `Common/Prey/CryAction/CryAction.h` identifies those two native
entry points and the delayed-save fields. The archived initial disassembly is
`PreyCoop_Tempfs_Archive_2026-09-04/tmp-top-level/ccry_savegame.dis`; the replay
was independently inspected in `/home/null234/Downloads/prey/_re/PreyDll.dll`.
The callback regression checks the accepted deferred request does no checkpoint
work and that the eventual forced replay waits for its final write callback.

Forced serialization is also not the final disk-write boundary. Native v13's
internal snapshot logged `SaveGame returned 1 ... force=1` before its
`ActiveSave::WriteComplete`. External save A0 captured successfully, but the next
same-slot save A1 failed its immediate fingerprint guard and stopped the session;
the completed A1 file then had different bytes. The pending-save path therefore needs both signals and
hashes only on the next main update, outside the writer callback. A 30-second
deadline only reports failure; it never substitutes for a completion event.
The runtime evidence is retained under
`prey-pr-artifacts-20261110/checkpoint-live/v13-failed-save-runtime.txt`.

The v15 native retry reached matching final-write completion but still failed
fingerprinting: `ResolveReadableSaveFile` treated the native
`%USER%/SaveGames/Campaign0/slot/save.CSF` alias as a literal path. The resolver
now expands only that exact alias under the effective save root and does not
guess another campaign if the explicit file is missing. Final native candidate
validation remains separate from the filesystem regression.

Final V21 native Windows verification passed that boundary: real QuickSave
logged deferred `force=0`, forced replay `force=1`, matching
`campaign0/quicksave0` final-write completion, then checkpoint publication, in
that order. The changed native file fingerprint matched the manifest, and both
the frozen and live account files contained the accepted inventory count 1001.
The same build also passed same-slot rollback, hard Client restart and combined
outbound-drop/write-failure recovery. See `PLAYER-CHECKPOINT-RECOVERY.md` for the
full native matrix, evidence entry points and explicit limits.

## Remaining Native Boundary

A checkpoint freezes Client bytes already known to the Host when native final
write completion is processed on the main update. It cannot include a Client
mutation not yet uploaded. The upload
requested after that save refreshes mutable reconnect state, not the immutable
checkpoint. Time-exact cross-peer saving would require a separate capture barrier
and acknowledgements before the native save; these tests do not claim that.
