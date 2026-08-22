# Auto DJ planning layer (isolated, pre-rebase)

This directory holds the core, conflict-isolated implementation of the
"final Auto DJ layer": a bounded queue, bounded history, explicit state
machine, and the `DjAutoDjPlanner` that ties them together. It is written
and tested standalone, deliberately without touching any file that other
in-progress branches (Coach/transitions, DjSession authority, physical and
browser control banks, library index/metadata) are also editing.

## Files

- `DjAutoDjTypes.h` - shared POD types: `AutoDjIdentity` (stable
  generation + fingerprint, never a filesystem path), `AutoDjCandidate`,
  reason bitmask, state/fail-reason enums, and all capacity constants.
- `DjAutoDjQueue.h` - fixed-capacity (`AUTO_DJ_QUEUE_CAPACITY` = 32) up-next
  queue. Pinned (user-selected) entries are always served before planned
  (planner-selected) entries; each group preserves FIFO order. Supports
  dropping entries whose library generation has gone stale.
- `DjAutoDjHistory.h` - fixed-capacity (`AUTO_DJ_HISTORY_CAPACITY` = 24)
  ring buffer of recently-loaded tracks, used for repeat/artist/title
  cooldown exclusion. A hash of `0` means "unknown" and is never treated as
  a match, so missing metadata can never produce a false exclusion.
- `DjAutoDjStateMachine.h` - the explicit
  `Off / Armed / Running / Paused / Stopping / Complete / Failed` graph.
  Knows nothing about I/O; every method performs at most one transition.
- `AutoDjLoadPort.h` - the abstract seam between the planner and the rest
  of the firmware (submit/poll a stable-ID load, manual-takeover signal,
  trustworthy-duration signal, recording-failure signal, physical
  confirmation signal). This is what gets a real adapter once the Coach /
  DjSession / physical+browser control branches are rebased in.
- `DjAutoDjPlanner.h/.cpp` - orchestrates the above. `tick()` is the
  one-shot runtime transition engine (at most one state or queue mutation
  per call, so it can never spin). `selectNext()` is the deterministic
  candidate picker that stands in for the final Coach scoring model.

## Design invariants (see `tests/auto_dj_selfcheck.cpp` for the checks)

- Every playable track is addressed only by `AutoDjIdentity`
  (generation + fingerprint) - never a raw path.
- The queue is fixed-size and never grows dynamically; pinned entries
  always win over planned ones, FIFO within each group.
- Track selection excludes recently-played tracks and cooling-down
  artists/titles over a bounded window, breaks ties deterministically by
  stable identity (not array order), and never fabricates a harmonic/beat
  match when metadata is missing - only a flat, clearly-flagged fallback.
- Runtime progress happens only through `tick()`, which performs at most
  one command submission/poll per call, with at most one command in
  flight at a time.
- Auto DJ is only armable when the firmware reports a stable-ID load
  endpoint; otherwise arming fails permanently (`Failed` /
  `CapabilityDisabled`).
- A failed/timed-out load is retried up to `AUTO_DJ_RETRY_BUDGET` times,
  then the track is permanently skipped - never retried forever.
- Any manual takeover of transport/crossfader/rate/load/cue/loop pauses
  Auto DJ and cancels the in-flight command deterministically.
- A missing/untrustworthy remaining-duration reading pauses for the user
  instead of guessing a crossfade point.
- Auto DJ never starts or stops a recording; a recording failure only
  ever causes a pause/fail requiring the user to resolve it.
- `Stopping` lets an already-submitted hardware command resolve (or time
  out) before finishing to `Off`, instead of abandoning it silently.

## Building the self-check

Not yet wired into `CMakeLists.txt` on purpose (that file is being edited
concurrently elsewhere, and this layer is still pre-rebase). Build and run
directly:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    tests/auto_dj_selfcheck.cpp src/AutoDj/DjAutoDjPlanner.cpp \
    -o /tmp/auto_dj_selfcheck && /tmp/auto_dj_selfcheck
```

## Known limitations / follow-up (tracked for the post-rebase pass)

- `selectNext()` uses a placeholder scoring function, not the final Coach
  bpm/key/energy model - the surrounding exclusion/tie-break/fallback
  behavior is meant to survive that swap unchanged.
- `AutoDjIdentity` duplicates the shape of `DjTrackIdentity` +
  `LibraryIndex` generation rather than reusing them, since those live on
  the in-progress Coach branch. Reconcile on rebase instead of keeping two
  copies.
- `AutoDjLoadPort` has no production implementation yet; it needs an
  adapter over the authoritative `DjSession` command queue plus the
  physical/browser control state once those branches land.
- No CMake/CTest wiring yet - add `AutoDjSelfCheck` alongside the other
  self-checks once `CMakeLists.txt` stabilizes post-rebase.
