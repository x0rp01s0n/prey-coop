# Prey CoopPrototype

`CoopPrototype` is a Chairloader mod that adds multiplayer cooperative play to Prey (2017). Each process keeps its own native `ArkPlayer`; shared world, story, area, enemy and interaction results are synchronized through typed authority lanes rather than generic entity mirroring.

## Requirements

- The same Prey and Chairloader build on every player.
- The same `CoopPrototype` protocol/build on every player. Mismatches fail closed before gameplay.
- UDP reachability on the configured port, default `27015`. Internet play may require a firewall rule and router port forwarding on the Host.
- A separate Chairloader/Prey profile for each local test instance. Reusing one generated account ID in a session is rejected.

## Start A Session

1. Start Prey through Chairloader.
2. Select `Multiplayer` in Prey's main menu. The same entry is available from the pause menu.
3. Use `Profile` to set the player name, account mode/id and model. Danielle Sho, Morgan Yu M/F, Sylvain Bellamy, Grant Lockwood and Mariana Arias have embedded in-game portraits.
4. The Host uses `Host Game` to choose a name, capacity from 2 to 16, port, LAN visibility and open/password/allowlist admission.
5. Other players use `Server Browser` for discovered LAN hosts, favorites or recents. Direct IPv4 join remains available for routed Internet sessions.
6. Follow the bounded join overlay through connection, Host save, player state, world load and area readiness. Rejections and the no-response timeout return to the browser with a readable reason.
7. Use `Session` to view the roster. The Host can change admission policy or kick one player without ending the other connections. `Leave Session` returns a Client to the main menu without closing Prey.

Names are presentation only. Inventory and reconnect ownership use a persistent `PlayerAccountId` stored in the local profile. Changing the displayed name does not create a new player.

## Configuration

The versioned profile configuration is written to:

```text
Saved Games/Arkane Studios/Prey/CoopPrototype/coop_config.xml
```

It stores the generated account ID, player name/model, Host settings, favorites, recents, last address/port, HUD/nameplate settings and friendly-fire policy. A corrupt file is quarantined and rebuilt with a new generated UUID. Platform identity is selectable in Profile; generated UUIDs are the safe default for local multi-instance testing.

## Ownership Model

- `PlayerOwned`: inventory, abilities, equipment, status and survival values belong to one account and one Host save key. A received player replacement is selected by exact account token and enters immediately before Vanilla's native `PostSerialize` reference/equipment callbacks; the mod does not transplant a custom GameState graph.
- `StoryOwned`: objectives, global facts, conversations and irreversible campaign choices use reliable idempotent events.
- `AreaOwned`: devices, hazards, GLOO, containers and physical results use exact GUIDs plus area leases/journals. Ordinary door/kiosk interaction runs vanilla exactly once on the interacting peer; unowned traversal callbacks cannot press the kiosk again. Shared moving-geometry controls such as the Moon Door and main lift route their input to current Area Authority. Doors are not exclusively claimed, and open/powered/locked replicate roundhouse as one atomic state. Loose carryable props, including `ArkLight` archetypes explicitly marked `bIsCarryable`, use an exact transform/velocity handoff and bounded settle lane.
- `EnemyOwned`: the Host owns the roster and routes leases; current enemy/area authority runs vanilla AI, listens to it and publishes its paths, locomotion and authored movement actions. Observers cancel their own path and follow accepted transforms. With local suspicion/attention, an observer's native combat AI may own look, facing, aim, aggression and attacks toward its real player, while the movement gate preserves authority-only pathfinding, root displacement and legs. Authority transfer restores exact captured physics. Mimic/Operator lunges retain authority movement without Phantom dash particles; only Phantom shift/dash actions use those effects. Mimic disguise, hacked/corrupted status and faction/IFF are durable and late-join repairable.
- `LocalPresentation`: cameras, HUD, audio, particles and cinematic helpers run locally from synchronized semantic results.

High-rate traffic is limited to compact player/enemy pose and actively moving prop state. Persistent changes are reliable events; projectiles, particles and arbitrary engine entities are never streamed as raw transforms.

## Troubleshooting

- **Protocol mismatch:** install the same mod build on every player.
- **Duplicate PlayerAccountId:** use a separate Prey/Chairloader profile or remove the copied `coop_config.xml` from one profile.
- **Incorrect password, allowlist or full server:** correct the value reported by the join overlay or ask the Host to change admission in `Session`.
- **Join timeout:** verify the Host is listening, the IPv4 address/port are correct, and UDP traffic is allowed by the firewall/router. A silent endpoint fails back to the browser instead of loading indefinitely.
- **World/save not ready:** let the Host finish loading before joining. Reconnect uses the Host's exact save key and area snapshot.
- **Player state corrupt for exact save:** restore that account/save state from backup, or remove only the reported exact-save state to create an empty player slot. The Host never substitutes another save's inventory or progression.
- **GPU/VRAM startup failure during local multi-instance testing:** terminate stale Prey/Proton/Wine processes before retrying. Do not repeatedly relaunch into exhausted VRAM.
- **Missing Host geometry during local multi-instance testing:** stop all test instances and rebuild that profile's shader cache. Each test prefix must use its own cache directory; sharing Steam's live `shadercache/480490` between simultaneous renderers can corrupt or starve the Host cache.
- **Diagnostics:** the large developer window and verbose traces are off by default. Enable them only for a bounded reproducer; normal play should not emit sustained console spam.

## Build

The repository's Xwin/MSVC Release build is:

```bash
podman run --rm --security-opt label=disable \
  -v "$PWD:/work" -v "$PWD/_xwin-cache:/root/.cache/cargo-xwin" \
  -w /work prey-msvc-buildtools \
  bash -lc 'ulimit -s unlimited; cmake --build _build/coop-xwin --target CoopPrototype --config Release -j 1'
```

The unlimited compiler stack is required for the current large `ModMain.cpp`
translation unit. Load `CoopPrototype.dll` through Chairloader's mod DLL
loader. Every participant must use the same protocol build.
