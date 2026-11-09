# Native Door Stress Test

`Run-DoorStress.ps1` drives two already-connected Windows sandbox processes
through the existing file-based runtime control interface. It does not launch
games, replace DLLs, or modify game packages. Use disposable profiles/saves.

## Fixture

- Both peers must be in Hardware Labs (`campaign/research/prototype`), in the
  same world epoch and outside loading guards.
- A `CoopRuntimeExtractor` command/status directory must exist in each profile.
- `ManifestPath` is an XML document with an `Entities/Entity` list extracted
  from the vanilla Hardware Labs mission. Entity attributes used are `Name`,
  `EntityGuid`, `Pos`, and `Rotate`.
- Defaults cover a regular sliding door and `LaserLabDoor` (Q-Beam room).
- Clear points remain near the tested door, looking toward it, 75 cm beyond
  the native trigger's depth boundary. Door approach points derive from authored
  positions and quaternion rotations. This runner is deliberately area-specific.
  Keeping the door visible avoids confusing deferred offscreen animation poses
  with a failed network edge; far-away closing needs a separate rendering test.
- No other harness may write to either profile's command file during a run.
  A named mutex excludes this runner's other instances in the same Windows
  session, not unrelated scripts or runners in another Windows session.

```powershell
.\Run-DoorStress.ps1 `
  -ProfilesRoot 'C:\Sandbox\profiles' `
  -GameRoot 'D:\PreySandbox' `
  -ManifestPath 'D:\Fixtures\hardware-doors.xml' `
  -ResultRoot 'D:\Results' `
  -Cycles 5 -Seed 247 `
  -ExpectedDllHash '<sha256>'
```

`GameRoot` contains `host` and `client` game copies, `ProfilesRoot` their
respective profile folders. DLL hashes must match. An optional expected hash
prevents accidentally testing the wrong candidate. `-PreflightOnly` checks
processes, hashes and fresh session status without mutating gameplay.

## Cases Per Door And Cycle

Vanilla must originate proximity Open/Close decisions, including the native
close timer. The co-op range predicate only gates existing callbacks/network
events: it must never generate Open/Close from a per-frame occupancy change.
Network replay applies the originating native event without republishing it.
When a live incoming Close is rejected while the local player still occupies
the native trigger, that peer publishes one fresh Open snapshot after processing
and relaying the Close. This confirmation does not call Open locally or reset
its timer; receiving an Open never generates another confirmation.

1. Client alone enters, holds briefly, then leaves.
2. Host alone enters, holds briefly, then leaves.
3. Both approach from opposite sides; client leaves while host remains for
   longer than the native close timer; finally both leave.
4. Client leaves and re-enters near the native close deadline.
5. Host repeats that re-entry case.
6. Host changes a different door's lock state immediately before the client
   approaches the target. This exercises unrelated-device echo suppression.
   The noise door's original lock is restored.
7. With both players away, invoke the client's native Open callback. Its
   network echo must not open the empty host door; native closing must settle.
8. While the client remains in range, invoke native Close on the host. The
   close must be rejected and both doors must remain open.

Additional cases can be selected explicitly with `-Cases`:

- `script_open_client`, `script_open_host`: native explicit Open with nobody
  in range must reach the other peer and then close natively. This tests the
  button/script callback path, not a physical button press or its FlowGraph.
- `script_open_held`: an explicit callback on an already proximity-open door
  must preserve its explicit cause on both peers after leaving. The native timer
  owns subsequent closing; there is no last-player-exits reconciliation.
- `boundary_client`, `boundary_host`: approach both depth boundaries at 15 cm
  outside and 5 cm inside the native box; compare occupancy, open state and
  collision on both peers. Dimensions come from the native probe, not guesses.
- `both_handover`: six alternating player handovers, with conflicting native
  Close attempts at each handover; the remaining occupant must keep it open.
- `floor_root_client`, `floor_root_host`: deliberately place the local root
  7 mm below the authored floor, then invoke native Close in the same command
  batch. This isolates point-versus-body occupancy; it is not a natural-walking
  reproduction or proof of a particular packet race.
- `expired_timer_exit`: remain inside beyond the native close timer, then leave;
  exercise native occupancy reevaluation while its timer is already expired.
- `burst_crossings`: twelve alternating entry/exit reversals without waiting
  for convergence, followed by a held entry and a final native close.
- `client_disconnect`: disconnect the only occupant, require the host to close
  with zero occupancy, rejoin, and repeat entry/exit. This uses the graceful
  disconnect path, not a process crash. `-JoinAddress` and `-JoinPort` control
  reconnection (default localhost:27015).
- `incoming_close_client`, `incoming_close_host`: require an external proxy
  delaying only PlayerPose in both directions by at least 1500 ms, recorded via
  `-ExpectedPoseDelayMilliseconds`. Explicit Open starts both doors, one peer
  enters, then the other closes before receiving the pose. Repeated samples on
  the occupant must remain open/unblocked; both then converge and close after
  leaving. These are pose/event ordering tests, not full-traffic loss tests.
- `incoming_proximity_close_client`, `incoming_proximity_close_host`: the same
  race, but the holder's native spatial entry opens both doors without an
  explicit/button baseline. Both must retain `explicit_open=0`, and the closer
  must still have no effective occupancy before its Close is attempted.
  Decision traces are needed to verify one confirmation, no local native
  Open/OnOpen call or timer reset during publication, and a correction accepted
  through sender-overlap evidence before effective pose occupancy catches up.

`-ReentryDelayMilliseconds 100` waits until the origin's native open flag first
becomes false, then returns 100 ms later. It does not assume leaving immediately
starts closing. This targets native close-animation re-entry; sampling may miss
part of a short animation. The default `-1` retains the native-timer deadline
scenario. The chosen delay is recorded in `run.json`.

`-TraceDecisions` enables a bounded, in-memory diagnostic trace for the current
door on both peers. Each case saves the native call stacks, occupancy inputs,
received pose targets, native area entity list and open/close decisions before
stopping the trace. It is off by default and can perturb timing; compare failures
with untraced runs rather than treating a traced pass as proof of absence.
With tracing enabled, a native proximity reopen from closed with zero current
occupancy fails the case even if the final samples later converge.

All cases require closed/blocked baseline, open/unblocked convergence on both
peers, and closed/blocked convergence after clearing both players. Close budgets
derive from the larger native timer duration plus four seconds. Nonzero trigger
occupancy is recorded, not silently deleted or assumed to be an invalid entity.
Initially locked test doors are temporarily unlocked through native callbacks
and restored afterwards. Cases 1-6 never force Open/Close. Cases 7-8 explicitly
exercise native callbacks to test rejection of empty network opens and occupied
closes; they do not write door fields or collision flags. The empty-open case
uses `proximity_open` to identify the native spatial callback context; ordinary
`open` is deliberately explicit and must work without an occupant.

## Evidence And Failure Behavior

Each run gets a timestamped directory with build PIDs/hashes, seed and manifest
hash in `run.json`; per-case `results.csv`; native samples in `samples.csv`;
full command responses; and `summary.json` with command response latency.
Failures additionally retain both peers' current status, bounded runtime
logs and the PowerShell error record. The summary distinguishes assertion
failures, cases blocked by a broken starting state, and harness errors; an
aborted runner is never a completed passing run. Independent case failures do
not stop later cases. Invalid initial
session/fixture or unavailable control fails the run instead of fabricating a
passing gameplay result. Any failed case yields a nonzero exit status.

Command latency includes polling, game-frame scheduling and disk I/O. It is not
a measurement of door hook CPU cost, network RTT, or framerate. Raw samples are
sequential rather than simultaneous; the runner allows a convergence budget
before comparing settled states.

The current native collision probe is one center ray and reports whether its
first hit belongs to the door. It is not a swept player-volume traversal test.
A missing door hit can also be caused by another collider intercepting that
ray; treat closed/no-hit discrepancies as investigation candidates, not proof
that the door's collider was removed. Native open-state disagreement is
independent of that limitation.

The extractor can write status from inside a command handler, before publishing
the final batch result. Modification time alone is not a completion signal.
The runner alternates two- and three-command batches, prefixing the requested
operation with read-only `coop_status` calls. It accepts only the changed final
batch count plus the expected final action. Thus each requested mutation still
executes once. It also requires the final `export=` line and keeps polling past
an unrelated action, rather than treating an intermediate response as a reply.

## Scope

These are native spatial-trigger and collision tests using the real player
entities, not keyboard walking, visual rendering checks, puzzle completion,
or general save persistence. Packet impairment requires an external proxy;
the runner does not inject it. Record its actual settings and packet evidence
alongside `-NetworkCondition`; that label alone does not prove impairment.
The optional rejoin case
covers only the listed door behavior. The runner raises test player
health to 3000 and moves them; never point it at an active campaign session.

Normally both peers must report interactive gameplay. `-AllowHostMenu` is an
explicit diagnostic exception for a host whose native menu input mode remains
on the input stack after harness loading. It still requires connected,
world-ready, non-loading simulation and an interactive client, records the
exception in run metadata, and does not clear or rewrite the native input stack.
Such a run only demonstrates spatial state/collision behavior, not usability
of the host's player controls. A menu-exception run must not be reported as a
full interactive gameplay test.
