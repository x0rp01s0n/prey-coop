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
`-DCHAIRLOADER_COMMON_PATH=C:/path/to/Chairloader/Common`.

## License

GPL-3.0. See [`LICENSE`](LICENSE).
