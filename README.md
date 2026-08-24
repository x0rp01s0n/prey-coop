# Prey Coop

This is the standalone development repository for `CoopPrototype`, a
Chairloader multiplayer co-op mod for Prey (2017).

The mod keeps one native player and one independently restored player state per
account. Shared campaign, world, device, enemy and physics outcomes are routed
through explicit authority lanes. Product usage, ownership rules and current
limitations are documented in
[`CoopPrototype/README.md`](CoopPrototype/README.md).

## Repository Layout

- `CoopPrototype/`: mod source, embedded UI assets and user documentation
- `BuildTools/`: reproducible Clang/Xwin/MSVC container support
- `build-msvc.sh`: isolated Linux build entry point

Prey data, Chairloader binaries, generated DLL/PDB/MAP files, local saves,
runtime logs, caches, reverse-engineering dumps, test fixtures and private
engineering notes are deliberately not tracked.

## Build

The mod needs a Chairloader checkout because it compiles Chairloader's `Common`
sources as a static dependency.

### Chairloader SDK compatibility

The supported upstream Chairloader revision is
`97899fbd86ab183a08ed579d6f95ed40262c7e15`. The checkout must be clean by
default. During CMake configuration, its `Common/` directory is copied to
`${CMAKE_BINARY_DIR}/_deps/chairloader-sdk/Common`, supplemented with the small
compatibility overlay in this repository, and patched only inside that private
build tree. The supplied Chairloader checkout is never modified.

The Linux wrapper reports the source checkout's Git HEAD and `Common/` dirty
state before mounting `Common/` read-only in the build container. Native CMake
builds inspect the checkout directly. A revision mismatch, local SDK changes,
overlay collision, or patch applicability failure stops configuration.

For deliberate forward-compatibility testing, revision and cleanliness checks
can be relaxed while retaining overlay and patch validation:

```bash
COOP_ALLOW_UNSUPPORTED_CHAIRLOADER=ON \
CHAIRLOADER_COMMON_PATH=/path/to/Chairloader/Common \
./build-msvc.sh
```

The `CoopAbiSmokeTest` executable is compiled and linked before the mod DLL and
asserts the restored `ArkTurret` ABI, including `sizeof(ArkTurret) == 3720`.

On Linux with Podman:

```bash
CHAIRLOADER_COMMON_PATH=/path/to/Chairloader/Common ./build-msvc.sh
```

The script creates the `prey-msvc-buildtools` image when needed, downloads
Microsoft Detours into the ignored `_deps` directory, configures the
Clang/Xwin/MSVC Release build and writes:

```text
CoopPrototype/CoopPrototype.dll
CoopPrototype/CoopPrototype.pdb
CoopPrototype/CoopPrototype.map
```

Set `BUILD_JOBS` to change the compiler parallelism. The default is `1` because
the current `ModMain.cpp` translation unit is large. Set
`COOP_REBUILD_IMAGE=1` to rebuild the container image.

For a native Windows build, configure `CoopPrototype/` with CMake 3.25 or newer,
the repository's `CoopPrototype/vcpkg.json` manifest and
`-DCHAIRLOADER_COMMON_PATH=C:/path/to/Chairloader/Common`. Use
`-DCOOP_ALLOW_UNSUPPORTED_CHAIRLOADER=ON` only for intentional compatibility
testing against another revision.

## License

GPL-3.0. See [`LICENSE`](LICENSE).
