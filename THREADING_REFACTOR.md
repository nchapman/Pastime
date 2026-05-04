# Threading refactor — `downplay_metadata` worker

## Context

`downplay_metadata`'s current design (`89a3a70529`) runs CRC32 + libretrodb
queries synchronously inside `downplay_drive_system_thumbnails`, which is
called every frame from the menu's render callback. The pump's `max_ops`
parameter caps the *count* of operations per frame, not the *time*. A
single multi-hundred-MB ROM CRC blows past the frame budget regardless:

| File size | Storage | Approx CRC time |
|---|---|---|
| 5 MB SNES ROM | microSD ~30 MB/s | ~170 ms |
| 64 MB N64 ROM | microSD ~30 MB/s | ~2.1 s |
| 700 MB PSX ISO (cart-likes path)¹ | microSD ~30 MB/s | ~23 s |

¹ Disc media (`.cue` / `.gdi` / `.chd` / `.iso` / `.bin`) routes to
serial extraction, which reads ~100 KB of header data. Only the cart-
likes path does whole-file CRC. Worst case in practice is large
homebrew / ROM hacks that ship as flat binaries.

The **subject-matter-expert review** identified this as an Important
finding ("UI freeze, not data loss, but the kind of thing the
'1-2 ops/frame budget' comment in the plan claims is mitigated when in
fact it isn't"). A naïve mitigation — capping CRC by file size — would
trade a UI freeze for a silently-skipped match. The real fix is to move
all the I/O off the menu thread.

This document captures the design as reviewed by the SME. **Read it in
full before editing the implementation** — several invariants are
non-obvious and the code-review will check for them.

## Goals

1. **The menu thread never blocks on file I/O or libretrodb queries.**
   `downplay_drive_system_thumbnails` becomes a bounded-time operation
   regardless of ROM size or storage performance.

2. **The public API is unchanged.** `downplay_index_open`,
   `downplay_index_lookup`, `downplay_index_note_present`,
   `downplay_index_finish_scan`, `downplay_index_set_art_state`,
   `downplay_index_pump`, `downplay_index_close` keep their existing
   signatures and behaviours from the menu driver's perspective. Only
   the *internals* and *timing* change.

3. **`HAVE_THREADS=0` keeps working.** Non-threaded console targets fall
   back to the existing synchronous behaviour without parallel code
   paths.

## Non-goals

- **Cancelling an in-progress CRC.** A 1-3 second back-button delay when
  the worker is mid-large-file is acceptable for the target hardware
  (Android handhelds, average ROM ~5 MB). Adding cancel-tokens would
  require either a custom intfstream VFS wrapper or forking
  `intfstream_get_crc` to take a poll-flag — both ugly. Revisit only if
  users actually report this.

- **Parallel matching.** One worker per index is enough; multiple
  workers would contend on the shared libretrodb cursor with no
  throughput win (queries are CPU-bound on a single linear scan).

- **Generalising the worker into a shared utility.** This is single-use
  scaffolding. The closest precedents (`audio/audio_thread_wrapper.c`,
  `gfx/video_thread_wrapper.c`) are also bespoke.

## Architecture

```
                 ┌──────────────────── menu thread ────────────────────┐
                 │                                                     │
  filesystem  ─► │ note_present(basename, mtime, size)                 │
  scan           │   → push work item by value into queue              │
                 │   → scond_signal(cond)                              │
                 │                                                     │
                 │ pump()  (per-frame, drain-only)                     │
                 │   → drain results queue                             │
                 │   → look up entry by basename + (mtime, size)       │
                 │   → apply if still valid                            │
                 │                                                     │
                 │ close()                                             │
                 │   → set shutdown flag, signal cond                  │
                 │   → sthread_join(worker)   ← may block ~seconds     │
                 │   → libretrodb_close, free everything               │
                 │                                                     │
                 └─────────────────────────────────────────────────────┘
                                       │ shared state
                                       │  (mutex-protected)
                                       ▼
                 ┌────────────────── worker thread ────────────────────┐
                 │                                                     │
                 │ for (;;) {                                          │
                 │   wait on cond until queue non-empty OR shutdown    │
                 │   if shutdown: break                                │
                 │   pop work item by value (under lock)               │
                 │   release lock                                      │
                 │                                                     │
                 │   CRC32 / serial extraction (seconds for big files) │
                 │   build CRC or serial query                         │
                 │   libretrodb_cursor_open + read first item          │
                 │   extract `name` field                              │
                 │                                                     │
                 │   acquire lock                                      │
                 │   push result by value into results queue           │
                 │   release lock                                      │
                 │ }                                                   │
                 └─────────────────────────────────────────────────────┘
```

### Shared-state ownership zones

```c
struct downplay_index
{
   /* === main-thread-only === */
   char *json_path;
   char *system_folder;
   bool  dirty;

   /* === shared (mutex-protected for read AND write) === */
   slock_t    *mutex;
   scond_t    *cond;
   dp_entry_t *entries;     size_t entries_count, entries_cap;
   dp_work_t  *queue;       size_t queue_count,   queue_cap, queue_head;
   dp_result_t *results;    size_t results_count, results_cap;
   bool        shutdown;

   /* === worker-only after sthread_create, until sthread_join === */
   /* Initialised on main thread before sthread_create publishes the
    * worker; closed on main thread after sthread_join.  Read-only by
    * the worker.  No lock required because thread create/join are
    * full memory barriers (POSIX requires it; rthreads.c delegates
    * straight through). */
   libretrodb_t *db;
   char         *system_root;
   char         *db_name;

   /* === lifecycle === */
   sthread_t *worker;       /* NULL when HAVE_THREADS=0 OR not yet started */
};
```

The current `dp_entry_t` keeps its shape but with a stricter rule:
**`label`, `match_value`, `match_kind`, `art_state`, `art_checked_at`
are written ONLY by the main thread.** The worker reads neither —
results land in the `results[]` queue, the main thread applies them.
`basename`, `mtime`, `size` are immutable after the entry is created
(`dp_entries_create`), so the main thread sees a stable snapshot and
the worker doesn't read them at all (work items carry copies).

### Work items and results carry full snapshots

Both queue and results structs are POD — no heap pointers — so
copying them across threads requires no locking beyond the queue
mutex itself, and there are no lifetime concerns.

```c
typedef struct
{
   char    basename[NAME_MAX_LENGTH];   /* canonical key */
   int64_t mtime;                       /* validity stamp */
   int64_t size;
} dp_work_t;

typedef struct
{
   char                      basename[NAME_MAX_LENGTH];   /* canonical key */
   int64_t                   mtime;                       /* validity stamp */
   int64_t                   size;
   enum downplay_match_kind  kind;
   char                      match_value[64];             /* hex CRC or serial */
   char                      label[NAME_MAX_LENGTH];      /* "" if unmatched */
} dp_result_t;
```

**Why basename + (mtime, size) instead of `entry_idx`**: the entries
array gets compacted by `finish_scan`, so an entry's index in the
array changes over time. A work item submitted before compaction would
land at the wrong slot if validated by index. Basename is the
canonical key; `(mtime, size)` is the validity stamp that catches a
file replaced under the same name. Result-application becomes:

```c
e = dp_entries_find(idx, result.basename);
if (!e)                         continue;   /* removed */
if (e->mtime != result.mtime)   continue;   /* file changed */
if (e->size  != result.size)    continue;
/* now apply label/match_kind/match_value to e */
```

`dp_entries_find` is O(N) over a small N (typically <1000 ROMs per
system); negligible at the per-frame drain budget.

## Lock discipline rules

The mutex protects:
- `entries[]`, `entries_count`, `entries_cap` — *all* mutations and
  *all* worker-relevant reads. Main-thread-only reads outside the
  shared subset (label/match/art) do **not** need the lock — those
  fields are not touched by the worker.
- `queue[]`, `queue_count`, `queue_cap`, `queue_head` — both threads
  push and pop.
- `results[]`, `results_count`, `results_cap` — worker pushes, main
  pops.
- `shutdown` — main writes, worker reads.

Functions that **must** take the lock for their entire body:
- `note_present` (mutates entries[], queue)
- `finish_scan` (mutates entries[] via compaction, rebuilds queue)
- `flush` (reads entries[] while serialising to JSON; we don't want a
  partial entry mid-resize)
- `pump` (drain-only, but the drain mutates results[] and applies into
  entries[])
- `close` (reads `shutdown`, writes it, then signals; releases before
  join)

Functions that **may run lock-free**:
- `lookup` — reads only the main-thread-owned fields
  (`label`/`match_value`/`art_state`) plus the immutable
  `mtime`/`size`. Doesn't touch `basename` storage (which is also
  immutable post-creation). The `entries[]` array itself can resize
  under us, though — so `lookup` needs a quick lock around the
  `dp_entries_find` walk to avoid racing a `realloc`. Cost is one
  uncontended mutex acquire per visible row per frame; trivial.
- The match worker's CRC + libretrodb cursor work — no shared state
  touched, only the snapshotted work item.

**Signal under the lock, not after release.** rthreads passes through
to `pthread_cond_signal`, which is safe either way per POSIX. Holding
the lock across the signal is the simpler model to reason about (no
lost-wakeup edge cases on stale state) and the perf difference is
irrelevant at our scale.

## Public API changes

None. Implementation-detail changes only.

The `max_ops` parameter on `downplay_index_pump` keeps its name but
its meaning shifts from "do up to N CRC+query cycles synchronously"
to "drain up to N results from the worker's queue and apply them to
entries". Behaviour from the caller's perspective is the same: bounded
work per frame, returns immediately.

## `HAVE_THREADS=0` fallback

rthreads' `slock_*` and `scond_*` functions are no-ops when threads
are unavailable (`libretro-common/rthreads/rthreads.c` returns NULL
from `slock_new`, and `slock_lock(NULL)` is a documented no-op).
`sthread_create` returns NULL too.

This means we get a single code path:

- `downplay_index_open` calls `slock_new` / `scond_new` / `sthread_create`.
  When HAVE_THREADS is undefined, `worker` ends up NULL.
- `note_present` always calls `slock_lock` (no-op when no thread) and
  `scond_signal` (no-op).
- **The pump branches**: if `idx->worker == NULL`, it runs the matching
  work inline (the existing synchronous logic from `89a3a70529`,
  preserved as `dp_match_one_inline`). If `idx->worker != NULL`, it
  drains the results queue.

The synchronous-when-no-worker fallback isn't a parallel implementation
— it's the same per-file CRC + query helper the worker thread calls,
just invoked from the pump rather than from the worker loop. One
function, two callers.

## Implementation steps

In rough order. Each step should leave the build clean and the unit
tests passing.

1. **Add work-item and result struct definitions.** Type-only, no
   behaviour change.

2. **Convert the queue from `size_t entry_idx` to `dp_work_t` by
   value.** Update `dp_queue_push` to take a `dp_work_t` (built from
   the entry by `note_present`). Update `dp_queue_pop` to return a
   `dp_work_t` by value. Update the existing synchronous pump to use
   the new work item. Verify all 95 disambig + 61 nav tests pass; run
   on device. **Commit.**

3. **Replace the `memmove`-based FIFO with a head-index lazy compact**
   (`queue_head` cursor, compact when `head > count/2`). Pure
   refactor, mechanical. Run tests. **Commit.**

4. **Add `dp_result_t` queue and the `apply_result` path.** Currently
   the pump applies results inline as it computes them; introduce a
   results buffer between compute and apply, even before the worker
   exists. Compute → push to results → main thread immediately drains.
   Behaviourally identical, prepares the seam. **Commit.**

5. **Add the worker thread, mutex, condvar, and shutdown flag.**
   Worker loop runs the CRC + query the pump used to do. Pump becomes
   drain-only when worker exists (preserved as inline path when not).
   Lock all `entries[]` mutations including the `finish_scan`
   compaction. Add the basename-based result validation in
   `apply_result`. **Commit.**

6. **Annotate the struct fields by ownership zone** (the comments
   shown in the Architecture section). Add `retro_assert(!sthread_isself(idx->worker))`
   debug guards on the main-thread entry points (`pump`, `flush`,
   `note_present`, `finish_scan`, `set_art_state`, `close`). **Commit.**

7. **Add a bounded queue cap** (~10 000 entries max) as a sanity net
   against runaway re-enqueueing bugs. Drop with a `RARCH_WARN`. **Commit.**

8. **Re-verify on device.** Drop a deliberately-large ROM (or an N64
   homebrew flat binary) into a system folder. Confirm the system view
   stays at 60 fps while the worker chews through the CRC in the
   background, results land within seconds, and back-button still
   responds within ~1-2 seconds (the last pop-and-join wait is
   acceptable per the non-goals).

## Risks and what could go wrong

### Critical: races during `entries[]` mutation

The biggest danger is forgetting to lock somewhere `entries[]` is
mutated. The SME explicitly flagged `finish_scan`'s compaction and
`flush`'s read pass. Mitigations:
- The struct comment groups fields by ownership zone, so anyone editing
  the struct sees the lock requirement at the point of declaration.
- Debug `sthread_isself` asserts on main-thread entry points catch the
  inverse error (a function being called from the wrong thread).
- The result-application uses `dp_entries_find` rather than direct
  index, so even if a result lands during a compaction, it
  self-corrects.

### Important: lifetime of the libretrodb handle across thread handoff

The handle is opened on main thread in `downplay_index_open` BEFORE
`sthread_create`, which is a full memory barrier (POSIX requires
`pthread_create` to synchronise-with the new thread's first
instruction). The worker uses it freely. On `downplay_index_close`,
the main thread signals shutdown, joins the worker (another full
barrier), then closes the handle. Single-threaded use throughout.

The implementation must NOT call `libretrodb_*` from the main thread
after the worker is started. If a future change adds a "look up by
CRC right now" debug path, it must do so before the worker spawns OR
take the mutex (which doesn't actually protect the cursor — it'd be a
hack). Better: keep this invariant as a hard rule.

### Important: shutdown without a final result drain

The worker may produce one final result *after* `shutdown=true` (it
finishes its in-progress CRC, locks, pushes, unlocks, loops, sees
shutdown, exits). The main thread must drain `results[]` BEFORE
freeing it on close. Current pump's drain-on-close behaviour handles
this; just make sure the close sequence is:

```c
1. lock; shutdown = true; unlock
2. signal cond  /* wakes worker if waiting */
3. join worker  /* may take seconds for large-file CRC */
4. drain results  (or just discard — they'd be applied to a
                   soon-to-be-freed index anyway)
5. close libretrodb
6. free queue, results, entries, mutex, cond
```

### Suggestion: cancellation latency UX

The 1-3 second back-button lag during a large-file CRC is acceptable
per the non-goals, but worth surfacing in a code comment so future-us
doesn't try to "fix" it without understanding the tradeoff.

## Testing

### Unit (mandatory, must pass before commit at each step)

- `downplay/tests/test_metadata_disambig.c` — 95 assertions, no
  threading involvement, must keep passing through every step.
- `downplay/tests/test_nav.c` — 61 assertions, unrelated but in the
  same suite, must keep passing.

The metadata index itself is harder to unit-test because its real-
world dependencies (libretrodb files, file system, atomic JSON) push
it into integration territory. We *could* add a `test_metadata_index.c`
that uses fake `.rdb` files and a tmpdir; the value-to-effort ratio
isn't great. Stick with disambig as the unit-test surface.

### Integration (on device; required before declaring done)

Run through these on the connected handheld:

1. **Cold start, large-ROM stress.** Place a 100 MB+ flat-binary ROM
   in a system folder. Open the system view. Frame rate stays at
   refresh; the row first shows the filename, eventually upgrades to
   the canonical label (or stays as filename if no match). No
   user-visible stall.

2. **Mid-CRC navigation.** Same setup, but back out of the system
   view while the worker is mid-large-file CRC. Acceptable: ~1-2 s
   delay before TOP renders. Unacceptable: hang, crash, or
   never-returning.

3. **Replace-in-place.** With the worker mid-CRC of `foo.smc`,
   `mv` a different ROM over `foo.smc` and re-enter the system view.
   The new (mtime, size) invalidates the in-flight result on apply;
   a fresh enqueue produces the right label.

4. **High-churn navigation.** Rapidly enter/exit several system views
   in <2 s each. Workers spawn, get told to shut down, join. No
   crashes, no leaks (Valgrind / ASan if available; otherwise just
   monitor `adb shell dumpsys meminfo com.retroarch.aarch64`).

5. **HAVE_THREADS=0.** Build a desktop variant with threads disabled
   (`./configure --disable-threads`) and confirm the synchronous
   fallback still produces correct labels — the existing pump logic
   should run inline when `idx->worker == NULL`.

6. **Stress: 1000-ROM folder.** Drop a No-Intro SNES set into a
   system folder. Open the system view. Within ~5-10 seconds the
   worker should chew through the queue and most rows should have
   canonical labels. Memory headroom: ~50 KB peak for the results
   queue.

## File touches

Anticipated diff (subject to implementation):

- `downplay/downplay_metadata.c` — substantial rework
- `downplay/downplay_metadata.h` — possibly unchanged; if doc comments
  on the API need to clarify "now async", update there too
- No changes to `Makefile.common` / `Android.mk` — `rthreads.h` is
  already linked transitively
- No changes to `menu/drivers/downplay.c` — the public API is
  unchanged, the menu driver's drive function continues to call
  `downplay_index_pump` per frame as today
- `THREADING_REFACTOR.md` — this file; delete or move to
  `docs/archive/` once the refactor lands

## References

- SME critique: see the conversation in `~/.claude/plans/this-looks-great-turn-functional-lollipop.md`'s history and the in-conversation review summary above commit `89a3a70529`.
- Closest in-tree precedents:
  - `audio/audio_thread_wrapper.c` — direct `rthreads.h` usage,
    similar producer/consumer + persistent worker pattern.
  - `gfx/video_thread_wrapper.c` — same shape, more state.
- libretro-common primitives: `libretro-common/include/rthreads/rthreads.h`,
  `libretro-common/rthreads/rthreads.c`.
- Synchronous baseline: commit `89a3a70529` "Downplay: per-system
  metadata index + box-art on hover".
