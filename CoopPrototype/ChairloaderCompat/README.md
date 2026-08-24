# Chairloader Compatibility Package

`prey-coop` builds against an **unmodified** upstream Chairloader checkout. The
supported base revision is recorded in [`base-revision.txt`](base-revision.txt).
At configure time, `PrepareChairloaderCommon.cmake` copies the supplied
`Chairloader/Common` tree into the private build directory, adds files that are
absent upstream, normalizes only the three patch-target headers to LF inside
that private stage, and applies the small patch in `patches/` there. The source
checkout is mounted/read as input only and is never patched in place.

## Package layout

- `overlay/Common/`: complete files that do not exist at the pinned revision.
  An upstream path collision is a hard error so this directory cannot silently
  shadow a newly added SDK file.
- `patches/0001-prey-coop-chairloader-sdk.patch`: minimal changes to existing
  upstream headers. Patch paths begin with `Common/` and are applied from the
  staged SDK root with `git apply --check` followed by `git apply`.
- `base-revision.txt`: the exact upstream Chairloader commit tested by
  `prey-coop`.

The mod-owned `CoopVTableHook` deliberately lives in `Src/`; Chairloader's
`VTableHook.h` and `VTableHook.cpp` are not changed by this package.

## Updating the supported Chairloader revision

1. Check out the candidate upstream Chairloader commit with a clean `Common/`
   tree and run the full prey-coop build plus `CoopAbiSmokeTest`.
2. Remove overlay files or patch hunks for fixes that upstream now provides.
   An overlay collision or failed `git apply --check` is an intentional rebase
   signal, not something to bypass.
3. Make only the remaining compatibility edits in the Chairloader worktree and
   regenerate the patch from the Chairloader repository root:

   ```bash
   git diff --binary -- \
     Common/Prey/GameDll/ark/turret/ArkTurret.h \
     Common/Prey/GameDll/ark/signalsystem/arksignalmanager.h \
     Common/Prey/CryMovie/IMovieSystem.h \
     > /path/to/prey-coop/CoopPrototype/ChairloaderCompat/patches/0001-prey-coop-chairloader-sdk.patch
   ```

4. Restore the Chairloader checkout, write the new full SHA to
   `base-revision.txt`, and verify that configuration stages and patches a clean
   checkout without `COOP_ALLOW_UNSUPPORTED_CHAIRLOADER`.
5. Confirm the source Chairloader worktree remains clean after configuration and
   compilation.

`COOP_ALLOW_UNSUPPORTED_CHAIRLOADER=ON` is for deliberate forward-compatibility
experiments only. It bypasses revision/cleanliness rejection, but overlay
collision checks and patch applicability checks remain mandatory.

## Note on this checkout's SDK patch (2026-08-24)

The originally supplied `0001-prey-coop-chairloader-sdk.patch` did not apply to
the pinned upstream revision `97899fbd`: its pre-image contexts were generated
against a header snapshot whose blank-line layout differs from the actual
upstream files at that revision (upstream has a blank line after the file
header comment in `ArkTurret.h` and tab-indented "blank" lines in
`arksignalmanager.h`; `git apply` requires exact matches).

The patch in this tree was therefore regenerated directly against the clean
pinned upstream checkout (`git diff` of the three target headers between
`97899fbd:Common/...` and the intended post-image), so it applies with plain
`git apply --check`. The resulting staged headers are byte-identical to the
intended post-image. Additionally, `CoopAbiSmokeTest` links `advapi32`
(required by CommonMod's registry helpers on Windows).
