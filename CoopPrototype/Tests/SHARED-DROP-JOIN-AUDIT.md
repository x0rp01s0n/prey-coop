# Shared-Drop Join-Window Audit

Status: verified source-level replay gap, native reproduction not yet performed.
This is separate from the escrow crash fix. The pre-session retirement and
missing public-drop replay already exist on main `1419eaaf1fd441b37d2a995040bca21005e59c9f`.
The new Host nonce check deliberately rejects stale sessions; it is not a replay mechanism.

## Verified Source Path

- `Src/CoopSharedDropSync.cpp:281`: public Spawn is sent when a native drop is observed.
- `Src/ModMain.cpp:79911`: ClientWorldReady can mark a peer gameplay-ready on the Host.
  The same handler can still queue its player-state transfer at `:79959`.
- `Src/ModMain.cpp:67743`: Host SharedDrop broadcasts include peers marked session-ready.
- `Src/ModMain.cpp:80736`: Client gameplay remains gated while awaiting Host player state.
- `Src/ModMain.cpp:68304`: a non-control payload received during that gate is retired.
  Its reliable sequence advances and is ACKed at `:68315`; retransmission cannot recover it.
- `Src/CoopSharedDropSync.cpp:879`: all commands, including Spawn, also require the current
  Host session nonce. A Client without that nonce rejects Spawn before materialization.
- `Src/ModMain.cpp:79920`: the ready-edge replay covers enemy roster and turret snapshots,
  not public SharedDrop records. Searching all `m_sharedDrops`/`BuildSharedDropPacket`
  references finds no initial/reconnect replay of the live shared-drop registry.

Some preloaded joins apply their player snapshot before WorldReady and avoid this window.
The fallback ready-then-player-state path is still present. A new public drop emitted after
the world snapshot was captured but before the Client can accept gameplay packets is not
covered by that earlier world snapshot or by a later inventory-only player snapshot.
Likewise, an existing live registry entry is not explicitly reannounced to a later peer.

## Next Targeted Test

Hold a joining Client before player-state completion, drop a distinct item on the Host,
and capture the pre-session reliable retirement. Finish joining, then compare both native
world inventories and the exact stable-drop registry entry. Repeat for an already-live
drop before a third peer joins and for area return. Do not claim native loss or a fix until
these cases are observed. A future change needs a scoped replay/receipt barrier rather
than weakening the nonce or relying on transport ACKs.
