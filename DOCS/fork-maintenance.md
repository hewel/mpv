# Maintaining the gpu-next host fork

## Ownership and baseline

`iced-player` is the long-lived product branch; `origin` is the hewel fork.
Published product history is merged forward, not rebased or force-pushed.
The initial upstream baseline is
`7e4cb538a3f30d25920ad8e87ba6571540fb729f` from
[mpv-player/mpv](https://github.com/mpv-player/mpv). The host extension is
`6430785cab693d103ea5a7b70de6efa92c5700d4`, directly on that baseline.
These are historical integration anchors, not instructions to follow a moving
branch. For each later synchronization, record the selected upstream commit and
its merge relationship in the synchronization PR.

Git commits are the source of fork changes. JellyPilot's source lock and build
manifest select the deployed combination; this document does not maintain a
second release-version list. The original measured experiment is recorded in
[gpu-next-host-baseline.rst](gpu-next-host-baseline.rst). Its temporary paths and
build commands are historical evidence, not production build prerequisites.

## Local extension and maintenance seams

| Seam | Purpose and invariant |
| --- | --- |
| `include/mpv/gpu_next.h`, `player/client.{c,h}` | Separate, borrowed host descriptor registered before initialization; independent host ABI version and explicit registration errors. Keep application UI and playback policy outside this API. |
| `video/out/gpu_next/host.{c,h}` | Import the host-owned Vulkan device and acquire/release host targets. Describe the actual enabled features/extensions, not merely supported features. Keep color processing in gpu-next/libplacebo. |
| `video/out/gpu_next/context.{c,h}` | Select the host backend only when explicitly registered; preserve the native backend otherwise. Host initialization failure must not silently select a native window. |
| `video/out/vo_gpu_next.c` | Reuse the existing renderer and playback scheduler. Successful target release belongs to scheduled `flip_page`, not early drawing completion. Do not fork a second copy of the renderer. |
| `meson.build`, `DOCS/client-api-changes.rst` | Build/install/export the interface and describe its changes. Preserve builds without Vulkan and the ordinary libmpv/native output interfaces. |

Before accepting an upstream change around these seams, inspect the shared
native path as well as the host path. A conflict-free merge is not evidence that
libplacebo import, texture state, renderer options, or scheduling still agree.

### ABI and resource contract

The authoritative contract is [gpu_next.h](../include/mpv/gpu_next.h).
`MPV_GPU_NEXT_HOST_VERSION` already provides independent versioning; registration
rejects a mismatched version before device import. No second version scheme is
needed. An incompatible layout, ownership, synchronization, or color-contract
change must bump that version and migrate the application together. The
structure has no size-negotiation field: appending fields is not implicitly
backward compatible. Device capabilities are checked during VO initialization,
not by successful registration alone; consume log/end-file errors as well as the
registration result. Without Vulkan, registration reports not implemented.

Key review obligations:

- Descriptor, callback state, enabled-feature chain and Vulkan objects outlive
  `mpv_terminate_destroy`; other client handles must already be gone.
- Every successful acquire has exactly one release, including error/teardown.
  Acquisition is bounded and prompt; a VO callback never waits for the UI.
- The host gives mpv exclusive access to a completed target. Only successful
  scheduled release permits presentation; host sampling/copy completion gates
  target reuse. A negative release is a discard with unspecified contents.
- All users of the shared native queue use the same synchronization. Host mpv
  calls, thread joins and waits for callbacks occur outside that gate. The iced
  gate's exact submission/callback policy belongs to iced and the application
  bridge, not a second implementation here.
- Fixed Linux/Vulkan SDR output is RGB10A2, BT.709/gamma 2.2/full range with the
  header's luminance and alpha conventions. Missing capability is an error, not
  an 8-bit substitution. No application-side replacement tone mapper.
- Release is permission to present, not scanout feedback. Software decode plus
  GPU rendering is not hardware decode or end-to-end zero-copy.

### Direct VAAPI on a host device

On Linux builds with VAAPI/DRM, the host backend queries
`VK_EXT_physical_device_drm` on the supplied physical device, resolves its render
node through libdrm, and verifies the opened character device's major/minor
identity. It owns that fd until host teardown, after hardware mappers and the VA
display have been destroyed. No application fd or host ABI change is required.
An unavailable identity or inaccessible node leaves hardware-decoder fallback
policy in effect; it never selects an arbitrary render node.

The consumer must enable supported `VK_KHR_external_memory_fd`,
`VK_EXT_external_memory_dma_buf` and `VK_EXT_image_drm_format_modifier` extensions
and their dependencies on the actual device it creates. Reporting their names
only in the host descriptor is insufficient. The existing VAAPI/libplacebo
mapper checks `PL_HANDLE_DMA_BUF` and probes decoded formats; availability of the
capability bit alone does not prove that every NV12/P010 modifier can be imported.
Verbose logs identify the render node and selected interop. Use `hwdec-current`
and the actual decoded image format to distinguish `vaapi`, `vaapi-copy`, and
software fallback in the embedded instance.

This reuses existing mapper synchronization and leaves scheduled target release,
host GPU completion, the consumer's private-texture copy, and color processing
unchanged. It does not establish end-to-end zero-copy. Changes to the isolated
iced example do not update JellyPilot's separately maintained device creation or
pinned artifacts; product and human color acceptance remain separate gates.

The host interface and any separable native-path fixes are candidates for
upstream discussion, not accepted upstream APIs. Before carrying a patch forward,
check whether upstream now supplies equivalent behavior; remove a local seam
only after the application migrates and joint acceptance passes.

## Controlled upstream synchronization

Do not merge merely because the fork is behind. Select a release or exact
upstream commit for a stated fix or compatibility goal. Keep unrelated iced/wgpu
upgrades out of an mpv/FFmpeg/libplacebo update unless a documented dependency
requires the combined migration.

In a clean, dedicated checkout, first inspect `git remote -v`. If no `upstream`
remote exists, explicitly configure it to
`https://github.com/mpv-player/mpv.git`; verify an existing remote rather than
silently replacing it. Fetch upstream, resolve the chosen reference to a full
commit, and create `sync/<descriptive-name>` from `iced-player`. For example,
after assigning `TARGET_SHA` to that verified commit:

```sh
git fetch upstream
git rev-parse --verify "$TARGET_SHA^{commit}"
git switch -c sync/upstream-candidate iced-player
git merge --no-ff --no-commit "$TARGET_SHA"
```

Choose a descriptive, unused synchronization branch name. Review and resolve each
conflict against both paths; do not bulk-select ours/theirs. Finish the merge
commit only after the relevant checks. Product integration must preserve this
upstream ancestry (ordinary merge, not squash). Commit, push and PR merge each
require the maintainer's authorization; a green report does not authorize them.
Do not rewrite a published synchronization branch to repair a failed trial.

`rerere.enabled=true` with `rerere.autoupdate=false` is an optional local aid,
not a requirement or an automatically applied setting. Reused conflict
resolutions still need review and behavior checks. A periodic trial may report
upstream compatibility without changing product pins; no scheduled auto-merge
is established by this document.

### Synchronization report

Record in the PR or review artifact:

- previous baseline and selected upstream commit; affected local commits/seams;
- conflict resolutions and changed internal contracts, including ABI decisions;
- native dependency/toolchain changes and why they must move together;
- commands, runtime environment and results, with unavailable checks explicit;
- application source-lock/build-manifest identifiers and the previous accepted
  combination to restore if joint acceptance fails.

## Joint acceptance and rollback

Use the application's maintained build and regression entry points rather than
copying a private SDK path or building against an arbitrary system libmpv. The
JellyPilot `README.md#fork-maintenance-and-joint-acceptance` and
`docs/agents/validation.md#opt-in-native-regressions` are the consumer-side
command sources; `README.md#manual-three-way-color-comparison` defines the
human color record. Keep historical measurements separate from new evidence.

1. Build native mpv and libmpv with the selected Vulkan dependencies and run the
   configured Meson tests. Verify the native output still plays; registration
   validation and a Vulkan-disabled build must continue to reject unsupported
   host use explicitly.
2. Exercise the real application bridge: playback, pause/seek, paused resize,
   target exhaustion/recovery, concurrent artwork upload, screenshot API,
   last-window close/reopen, stop confirmation and final teardown. Check queue
   deadlocks and growing retained resources, not just process exit status.
3. Include real tray initialization under a non-C user locale. Tray-free smoke
   misses GTK locale mutation and cannot establish libmpv startup safety.
   External playback/startup must remain independent of embedded dependencies.
4. On the actual GPU, compare old native mpv, candidate native mpv, and candidate
   embedded output. Use SDR, HDR10 and a metadata-confirmed Dolby Vision Profile
   5 sample; a generic HDR or synthetic sample does not prove Profile 5. Record
   source identity, fixed times, baseline settings, actual physical dimensions
   and output encoding. Compare actual surfaces, not an unrelated screenshot
   render path. Keep copyrighted media/reference images outside Git.
5. Record sustained playback, pause/resume and seek behavior, drops, mpv A/V
   statistics and resource growth. mpv A/V statistics do not measure display
   scanout. Explain color differences before accepting a new reference; never
   update references automatically to make a comparison pass.

A source/build check cannot replace unavailable GPU or human visual acceptance.
Restore the application's entire previously accepted source-lock/Cargo.lock and
matching native artifact/configuration manifest when rolling back. Do not move
published fork branch pointers or swap one native library while leaving the
other half of an unverified combination installed.
