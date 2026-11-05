# PreyCoop Performance Diagnostic Build

Experimental test build: current main + PR #60 + PR #61 + diagnostic instrumentation. Protocol 250. This is not a stable release, and the two PRs still have known unverified failure paths. Back up your co-op profile and saves before testing. Every player in the test session must use this same build; it cannot connect to the public protocol 247 release.

## Install and Run

1. Close Prey and ChairManager. Back up the existing `Mods/CoopPrototype` folder and your Prey save/profile folder.
2. Replace `Mods/CoopPrototype` with the `CoopPrototype` folder in this package.
3. Double-click `Run-Diagnostics.cmd` and select `ChairManager.exe`. Launch Prey from that ChairManager session with the mod enabled.
4. Keep normal graphics settings and use the same save for comparison. Other heavy diagnostic tracing should stay off. The launcher keeps `COOP_RUNTIME_OBJECT_PROBE_MODE=direct`, matching the earlier workaround.
5. Test solo for about one minute, then join the friend's session and remain idle for one minute, then move normally for one minute. Finally ask the host to use one recycler grenade on a modest item pile. Note which stage lags or freezes; do not risk valuable items.
6. Close Prey. Double-click `Collect-Diagnostics.cmd` and send the resulting ZIP with those observations.

Performance logs go to `Documents/PreyCoop-Diagnostics/<session>`. If collection says no performance log exists, the diagnostic DLL or inherited settings did not become active; do not send an old `Game.log` as a substitute.

The collection script only packs this session's performance files and launcher metadata, not inventory, save files, or crash dumps. Nothing is uploaded automatically. Review files before sharing.

## Optional CPU Trace

If the performance log does not identify the slowdown, run `Record-CPU-Trace.cmd` while the diagnostic game is already open. Accept the Windows administrator prompt, read the privacy notice, then press Enter and return to Prey. Reproduce the lag during the following 60 seconds. The script uses Windows Performance Recorder's built-in CPU profile; it does not install software or cancel an existing recording.

The resulting `Documents/PreyCoop-Diagnostics/cpu-*.etl` can be large and contains system-wide metadata such as process names and paths. Share it privately only when requested. It is deliberately excluded from the collection ZIP. This is optional; start with the ordinary performance logs.

## Return to the Normal Release

Close Prey and ChairManager and restore the backed-up mod folder. Launch ChairManager normally, not through the diagnostic launcher. The launcher's environment settings only apply to the processes it starts; it does not edit Windows environment settings or game configuration.

## Interpreting Results

Update timing is not a GPU presentation measurement. Inclusive scopes overlap and must not be added together. Measurements identify expensive paths; they do not by themselves prove why another machine is slow. A stalled main thread cannot write a summary until it resumes.
