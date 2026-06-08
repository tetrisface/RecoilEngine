# Replay Checkpoint Regression Tests

This document tracks regression cases for replay checkpoint restore. The goal is
to turn each desync we find into a focused test target, even if the current
engine shape requires smoke tests before smaller unit tests are practical.

Upstream design context and source links are summarized in
[`docs/replay-checkpoint-upstream-notes.md`](replay-checkpoint-upstream-notes.md).

## Acceptance Invariant

For a deterministic SYNCCHECK replay with checkpoint bundle:

1. Restore to checkpoint frame `R`.
2. Resume playback from `R`.
3. Every sync check from `R` onward matches the original replay path.
4. No `DESYNC WARNING`, sync-hash mismatch, or hidden fast-forward shortcut is
   accepted as success.

The scripted smoke path and the BAR replay timeline UI path both need to satisfy
this invariant.

## Current Smoke Fixture

Use the Windows scripts until the engine has smaller injectable seams:

```powershell
.\scripts\replay-timeline-record-synctest-windows.ps1 `
  -SpringExe .\RecoilEngine\build-windows\install\spring.exe `
  -DurationFrames 900 `
  -RequireSyncHash

.\scripts\replay-checkpoint-smoke-windows.ps1 `
  -RunDir .\.cache\replay-timeline-synctest\<run> `
  -SpringExe .\RecoilEngine\build-windows\install\spring.exe `
  -SaveFrame 90 `
  -LoadFrame 180 `
  -TargetFrame 90 `
  -ResumeFrame 181 `
  -LuaRulesSelfTest
```

This is an integration test, not a unit test, but it is currently the strongest
evidence because it exercises CREG load, Lua state, demo replay, frame
accounting, command handling, and sync checks together.

The Windows replay scripts mute both music and master sound while running:
`snd_volmusic = 0` and `snd_volmaster = 0`.

## Regression Cases

### COB Scheduler Wait State

Bug shape:

- `CCobEngine::waitingThreadIDs` was ignored by CREG.
- A frame-90 restore produced different waiting thread ids from the original
  path immediately after load.
- The next resumed frame diverged through different COB scheduling behavior.

Test target:

- Construct or fixture-load a `CCobEngine` with non-empty
  `waitingThreadIDs`.
- Save and load through CREG.
- Assert `waitingThreadIDs`, sleeping thread ids, and script thread ownership
  are preserved.

Future seam:

- A small CREG round-trip harness for individual synced components.
- No game window, map load, or demo reader should be required for this case.

Current status:

- Fixed in source by serializing `waitingThreadIDs`.
- Verified improvement: frame-90 original and restored dumps now preserve
  waiting and sleeping COB thread ids.
- Not sufficient for replay rewind correctness; later resume still desyncs.

### Ground Move Path Rebuild

Bug shape:

- `CGroundMoveType::PostLoad()` discarded the saved `pathID` and requested a
  replacement path.
- The old path request did not also rebuild `currWayPoint`, `nextWayPoint`,
  `earlyCurrWayPoint`, or `earlyNextWayPoint`.
- The first resumed frame diverged in unit movement state even when unit counts,
  feature counts, projectile counts, and RNG state matched.

Test target:

- Restore a ground unit with active movement, non-zero `pathID`, and a pending
  move command.
- After `PostLoad()`, assert the replacement path id and waypoint fields are
  internally consistent with the path manager result.
- Resume one sim frame and assert position, heading, progress state, and command
  queue match an uninterrupted replay path.

Future seam:

- Inject a deterministic fake path manager into `CGroundMoveType` so
  `PostLoad()` can be tested without a full map/pathfinder.
- Keep a higher-level replay fixture because real QTPFS behavior may still
  expose bugs a fake path manager cannot.

Current status:

- Still unresolved.
- A naive attempt to call `GetNewPath()` from `PostLoad()` was rejected because
  `GetNewPath()` has a near-goal early return that changes prior load behavior.
- A second attempt that preserved the old unconditional `RequestPath()` call but
  also refreshed waypoints changed the restored replay path and still desynced:
  restored frame 120 became `sync=15f5d983 units=375 features=10` while the
  original replay path was `sync=1c71c713 units=367 features=8`.
- A synchronous QTPFS rebuild from `CGroundMoveType::PostLoad()` was rejected:
  calling `RequestPath(..., immediateResult=true)` during CREG object postload
  crashed before checkpoint restore reached demo playback reset.
- Moving `pathManager->ResetLivePathsForLoad()` before CREG `LoadGame()` was
  also rejected: it avoided the crash but still desynced, with restored frame
  120 becoming `sync=d5dfa19e units=377 features=10`.
- A preferred-QTPFS-path-ID rebuild inside `CGroundMoveType::PostLoad()` was
  rejected. It ran before `CGame::LoadReplayCheckpoint()` called
  `pathManager->ResetLivePathsForLoad()`, so the rebuilt paths were then
  deleted. The run logged requested IDs being remapped, followed by:
  `reset QTPFS live paths for load: 333 paths, 116 searches`.
- Moving the preferred-ID rebuild after `ResetLivePathsForLoad()` was in the
  correct phase, but still not sufficient: entt did not recreate the same raw
  path IDs after the restored entities had been destroyed. Fresh fixture
  `.cache/replay-timeline-synctest/run_20260605_003246` restored f91 as
  `sync=fae37455` instead of original `sync=1947f604`; f120 desynced
  `fec3fd92 -> 87397fec`, and f180 desynced `6a761137 -> af2d1334`.
- Preserving CREG-loaded QTPFS `IPath` entities instead of deleting and
  re-requesting them avoided path ID remap warnings, but is still incomplete.
  It logged
  `reset QTPFS live paths for load: preserved 243 paths, removed 0 searches, cleared 0 search refs`,
  but fresh fixture `.cache/replay-timeline-synctest/run_20260605_004845`
  restored f91 as `sync=2fb3cda4` instead of original `sync=22ee2357`; f120
  desynced `7b1cced4 -> 1714b156`, and f180 desynced
  `9a4b946b -> 54e959ce`.
- The next test target should compare CREG-restored QTPFS path internals and
  `CGroundMoveType` waypoint/progress state after load, before replay resume.
  Path IDs alone are not enough.

### Replay Restore Frame Accounting

Bug shape:

- Checkpoint restore must leave the viewer at the restored frame without using
  server-side replay `skip f<frame>` as a fake backward jump.
- Pause state can be mutated by save-load internals and must be restored by the
  replay checkpoint path.

Test target:

- Request load from frame `L` to target frame `R`.
- Assert `gs->frameNum`, server frame accounting, demo reader frame, and Lua
  observed frame all report `R`.
- Assert the viewer remains paused after restore when it was paused before
  restore.
- Assert no catch-up simulation frames are processed as the mechanism for a
  backward jump.

Future seam:

- Extract a checkpoint restore context that owns frame accounting, pause
  preservation, and demo-reader positioning.
- Unit-test that context with fake game server/demo reader objects.

### Sync Hash Resume Equivalence

Bug shape:

- A restore can appear visually successful and still desync after resume.
- Frame-local state dumps may match selected subsystems while omitted state
  still diverges.

Test target:

- Record original sync signatures for every sync-check frame in a fixture.
- Restore from each checkpoint interval boundary.
- Resume past at least the next two sync checks.
- Assert original and restored sync hashes match exactly.

Future seam:

- Keep this as an integration regression test. It is intentionally broad and
  should remain the final gate for any replay checkpoint change.

### Checkpoint Bundle Integrity

Bug shape:

- A replay timeline may select the wrong checkpoint or load an incompatible
  checkpoint bundle.

Test target:

- Verify every bundle entry has frame, file, engine version, map, game/mod, and
  hash metadata.
- Assert nearest-checkpoint selection returns the latest checkpoint at or before
  the requested target frame.
- Reject checkpoints with incompatible engine/map/game metadata.

Future seam:

- Unit-test this in `recoil-replay-rs` and in the C++ resolver separately.
- Keep the current `replayctl verify-bundle` path as the command-line guard.

## Performance And Storage Tests

These tests belong after correctness is proven:

- Measure checkpoint save wall time relative to frame processing time.
- Assert keyframe saving adds less than 10 percent of frame processing time in
  the selected benchmark fixture.
- Assert checkpoint storage adds less than 50 percent over replay size, or no
  more than 2 MB, for the selected benchmark fixture.
- Assert the default interval is no worse than one keyframe per 5 minutes of
  game time.

Future storage work may parse `.ssf` internals and compare alternate encodings,
but no storage optimization should be treated as successful until restore/resume
sync equivalence is already green.

## Suggested Test Layers

1. Component CREG round-trip tests for individual state owners.
2. Restore-context unit tests with fake demo reader, fake server, and fake pause
   state.
3. Pathing/movement post-load tests with deterministic fake path manager.
4. Scripted SYNCCHECK replay smoke tests for end-to-end determinism.
5. BAR replay timeline UI smoke tests for the user-facing command path.

The smaller tests should catch known mistakes quickly. The SYNCCHECK replay test
remains the authority for whether rewind is actually safe.
