# PhotoPull core storage API

`core.h` is the protocol boundary. `ParseConfigJson` accepts the bounded
`photopull.json` document; `ParseManifestJson` accepts the bounded server
manifest and emits `Manifest::normalized_json`. The latter is deterministic and
contains the effective display interval in the in-memory structure, while its
JSON form preserves whether the server supplied `display_interval_sec`.

The manager should construct `Store` with an implementation of `FileSystem` and
the display component's `PhotoValidator` callback. The normal update sequence
is:

1. `Recover` during startup and display only `RecoveryState::valid_images`.
2. `PlanCandidate` using a source string composed from the normalized HTTPS
   origin and manifest path. The core normalizes this source again before
   comparing or persisting it. A `kNotModified` plan with missing files means
   an unconditional manifest request is required; a local `304` is never an
   integrity proof. `total_missing_bytes` counts each SHA only once when
   several ordered IDs reference the same image.
3. For every missing item, call `BeginImage`, feed bounded network blocks to
   `ImageWriter::Write`, and call `Finish`. The writer checks the exact length,
   SHA-256, validator, sync, and atomic SHA-name publication.
4. Call `Commit`. The first complete snapshot slot is the logical commit point.
   Pass `defer_mirror=true` when the manager must return immediately after that
   point; call `RepairMirror` from a later bounded work unit. The default keeps
   the host tests convenient by attempting the mirror inline.
5. `GarbageCollect` revalidates both slots as the same complete snapshot before
   deleting anything. `RepairMirror`/`Commit` invoke it after mirroring. The
   manager also retries `gc_pending` while idle, with 30 seconds between failed
   attempts, and before the next candidate's free-space check. On startup it
   first waits for an accepted 200/304 so it can retain the candidate's SHA
   files using `GarbageCollect(&candidate)`; rejected manifests do not replace
   that retention set. Active image writers prevent collection. `mirror_pending`
   means the peer slot still needs repair. Unknown files and files outside the
   managed directory are always retained. Cleanup success clears `gc_pending`;
   a failure leaves it set for a later retry.

When the newest structurally valid slot has a missing image but an older slot
is complete, `Recover` selects the complete slot for offline playback and
returns its `valid_images` mask. It also retains the newer slot in
`has_newer_incomplete`/`newer_incomplete_snapshot` as the repair and version
basis. `PlanCandidate` compares against that newer record, so a rollback is
still rejected and the same revision can be retried after its missing content
is downloaded. `RepairMirror` leaves this newer damaged slot intact; copying
the older playback snapshot over it would erase the repair basis. Once the
candidate is committed, both slots can be mirrored and garbage collected under
the normal complete-snapshot checks. When no complete slot exists, `Recover`
selects the newest structurally valid slot and exposes its partial
`valid_images` mask for repair. `RepairMirror` revalidates every image in the
current slot immediately before mirroring or cleanup.

`FileSystem::ReadFileChunks` is the streaming hook used for existing image
verification. The core does not take a FreeRTOS or SD mutex; the manager owns
serialization and can yield between bounded operations.
