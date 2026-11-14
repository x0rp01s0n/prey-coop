# Shared Drop Area Readiness Audit

## Follow-up On 2026-11-14

V36 reran the solo and split-area cases on current main, including native travel,
stop/rehost, offline collection/redrop, and repeated pickup/rejoin. Four hard Host
restart cases also passed: saved public sources from either player, a drop after
the checkpoint, and a pickup after the checkpoint. Native inventory quantities,
layout and visible world entities were the oracles. DLL SHA256:
`27b12922fd840a07ca73ddcd414259177fe159ed8fb01391e257ce002b9d5b55`.
Archive SHA256:
`9cd4272484e0b5051a1f0eadd66f18ef2ecf6f2676fb1d97ec0a300efdf86913`.
All 14 archived production source hashes match commit `d91f478`. The restore
initially refused a stale Host process pin after the final intentional restart;
the final boundary's exact pins were verified and the complete original
profiles/mods were then restored with matching path/size/SHA256 manifests.

The earlier V35 native run really lost a committed item after offline redrop:
zero in both inventories and no public source. V36 retires the acquired lifetime
before vanilla redrop and assigns a fresh identity. This is a shared-drop
lifecycle fix, not an additional journal.

V36 is not final proof for the later pending-remainder changes. A production
function regression subsequently found that transfer preparation discarded an
already-assigned, partially delivered receipt on a new transport session. That
case and serialization-point checkpoint staging still need native verification.
The PR remains on hold until those checks finish.

V32, protocol 258, fixes both V30 counterexamples below without another journal:
publication creates a fresh lifetime even when the Host is alone; ownership
coordination accepts admitted peers in other areas; native travel detaches only
entity bindings and retains the source catalog. Returning native sources are
rebound by their stable identity rather than republished as new drops.

The native Windows pair passed solo pickup, repeated pickup, redrop through the
normal F interaction, and live rejoin. It also passed Host travel to Lobby while
the Client stayed behind, Client pickup/redrop/re-pickup under area authority,
native Client travel to Lobby, and another rejoin. Inventories and native world
entities were inspected at each boundary. V32 DLL SHA256:
`cf019985022db9bd3d0a5e11f62e5fbb43a8eb8fd90171ae773c3f65a000e876`.
Evidence archive SHA256:
`ba2dc17ed42f47dca9e9ff4b11615bb2af304a2afe7caa3ea18a1f7ab69255df`.
Both games were stopped and the original isolated profiles/mods restored and
verified after that run.

Further selected-production tests exposed and fixed two follow-up cases:
duplicate saved entities bearing one source ID, and loss of the catalog when a
continuing Host stops/rehosts. Offline native inventory membership must retire
the catalog entry without deleting the inventory object. These additions still
need the final native run, including the earlier crash/rollback matrix and
integration with the now-merged PR61/PR62. Do not treat V32 evidence as proof of
those later changes. The original V30 failures remain recorded below.

## Candidate And Scope

Native Windows pair, protocol 257, V30 DLL SHA256:
`1f7be032d80613ad65ebdb4c78767da8016e9404deffba5e7cc28beea1de55a2`.
Both isolated installations were deployed while stopped. Every command checked
the exact process identity and deployed DLL hash. Native inventories and world
entities, not screenshots or a missing network record, are the result oracle.

The readiness correction allows a loaded session Host to commit a local public
pickup without a ready remote peer. It preserves the local load/transition
fences. This is not a change from session Host arbitration to Area Authority
arbitration.

## Split-Area Counterexample

1. Both players start in Neuromod Division (`campaign/research/prototype`).
2. The Host makes a real native one-banana drop. Both players have a live,
   visible, one-item binding to the same stable source.
3. The Host uses the authored native transition door to the Lobby. The Client
   remains loaded in Neuromod Division and obtains the Area Authority lease.
4. Three native pickup attempts on the Client grant nothing. The source stays
   visible, live, unreserved, uncommitted, and count one. Client inventory stays
   zero. Both peers retain the same world epoch, save key and inventory timeline.

Observed source: `8942201403218311436`. Client entity: `130162`.
`gameReady=1`, `loadGuard=0`, `areaAuthority=1`, `sessionReady=0`.
The native command reports rejection rather than a successful transfer.

An independent repeat after resetting the fixture and preserving the requested
harness phase produces the same failure: source `6876708770539460736`, Client
entity `195688`, three native attempts, inventory zero before and after, one
visible/live/uncommitted world unit retained. The first run was also retried
five times before any return transition; it remained blocked with no grant.

This is a real functional gap, not evidence of item loss or duplication. The
current Client pickup gate requires same-area session readiness even when that
Client is the admitted local Area Authority. Removing that check alone is not a
fix: the session Host handler also validates the packet against its own loaded
area and needs a source record to reserve and commit. Host unload clears that
local record. Arbitration, routing and source lifetime across area handoff must
be handled together; do not bypass escrow and fall through to personal loot.

The original PR head also uses session-Host arbitration and local-area payload
validation. Whether this exact symptom is a new regression is not established
by the V30 gameplay test. It must not be presented as fixed in this candidate.

## Solo Host And Native Redrop

The corrected native solo run passes the primary boundary:

- Initial native counts: Host one banana, Client zero.
- Real Host drop, then graceful Client disconnect: Host zero, public source one.
- Three native Host pickup attempts: Host one, source committed with a stored
  one-unit receipt. Five further attempts do not grant again.
- Normal live Client rejoin: Host one, Client zero, no resurrected public source
  on either peer.

Source: `7156610001553251489`, native Host entity `64632`.

A further native lifecycle edge fails. After the Client leaves again, the Host
drops this acquired item normally. Native inventory decreases to zero and the
same entity is visible, not garbage, and not stored in either inventory. Its
normal F interaction is populated and actually invoked, but the Host still gets
zero items. The one-unit world source remains present.

The old committed binding still exists (`sharedDropState=0/0/1/1`). Drop capture
returns before retiring that binding when no remote peer is ready. Consequently
the native redrop does not get a new lifetime, and pickup finds the already
delivered receipt from the previous lifetime. This is a second functional
counterexample, not a demonstrated loss or duplication. Fix the source lifecycle
in the central drop path; do not add a recovery journal or ignore a committed
receipt in the pickup path.

## Test Provenance

The first harness run selected the split branch because an imported helper
overwrote its phase variable. The resulting native operations and observations
above are recorded, but it is not a solo-Host result. The harness now preserves
the requested phase separately. The corrected solo run above uses that fixed
harness. A read-only world sample also retried a transient entity identity
mismatch; the failed observation was retained and not counted as gameplay proof.

An attempted native return through the Lobby's Neuromod Division door loaded
`campaign/research/simulationlabs`, not the fixture's
`campaign/research/prototype`. The same-area return check therefore timed out.
This is an invalid return fixture, not evidence of a failed source rebind. It
does not invalidate the preceding observed split-area pickup refusal.

The selected-production transaction suite passes 55 controls. Native delivery
and persistence are faked dependencies in that suite; it is not split-area
gameplay proof. The earlier V28 crash/rejoin and rollback results remain valid
for their pinned candidate and do not establish V30 split-area behavior.

## Release Decision

Hold the PR update as merge-ready while the split-area pickup and solo redrop
gaps remain.
No new journal or forced inventory grant was added to hide this failure.

Both native instances were stopped and the original isolated profiles/mod files
restored using complete path/size/SHA256 manifests. The final source manifest
matches all ten corresponding local production files. The Windows and local
evidence archive hashes agree, and archive integrity verification passes:
`dfc63bb490b459175d71968efe588c1ef93b78b797ff3db9a28ac483c9e34864`.
