Linux Vulkan SDR host baseline
==============================

This is the historical experiment and acceptance record. For subsequent fork
updates, use the maintenance and joint-regression procedure in
`fork-maintenance.md <fork-maintenance.md>`_; do not reuse temporary paths below
as production build inputs.

Scope
-----

This prototype targets Linux/Vulkan SDR only. Native mpv remains the picture
reference. Neither CPU readback nor successful compilation constitutes GPU
embedding acceptance. Playback controls must not be migrated into the existing
application until matched picture comparison passes. No JellyPilot production
UI or playback business logic is changed by this experiment.

Measured reference (2026-09-09)
-------------------------------

* Executable: /usr/bin/mpv, mpv v0.41.0, built Aug 29 2026 22:02:52.
* FFmpeg: n9.0.1; libavcodec 63.1.101, libavdevice 63.1.101,
  libavfilter 12.1.101, libavformat 63.1.101, libavutil 61.1.101,
  libswresample 7.1.101, libswscale 10.1.101.
* libplacebo: v7.360.1, API 360.
* Fork starting revision: 7e4cb538a3f30d25920ad8e87ba6571540fb729f.
  This revision is not the installed reference executable; comparison must
  distinguish the installed reference, fork native output, and embedded output.
* Actual VO: gpu-next. Actual GPU API/context: Vulkan / waylandvk.
* Actual device: AMD Radeon RX 7900 XTX (RADV NAVI31), PCI ID 1002:744c.
* Decoder: hevc-vaapi; actual decoded image vaapi[p010]. Audio: PipeWire.
* Compositor reported no wp_color_manager_v1 or
  wp_color_representation_surface_v1 support. The measured output is SDR,
  notwithstanding target-colorspace-hint=yes in the user's configuration.
* Source: One Hundred Years of Solitude - S01E12 - The Innocent Train Had
  Arrived - [WEBDL-2160p][h265-10bit][EAC3 Atmos].mkv in
  /home/hewel/Codes/jellypilot/test-videos. Paused time-pos: 30 seconds.
* Actual window target: 961x541 physical pixels, requested with
  --autofit=960x540. Requested and actual sizes differ; comparisons must use
  actual physical target dimensions, not the nominal window request.

IPC video-target-params resolved the target to rgb10a2, RGB/full, BT.709,
gamma2.2, min-luma 0.203, max-luma 203, sig-peak 1, premultiplied alpha.
These are nominal rendering luminance conventions, not a measurement of the
physical monitor's white or black level. Input was 3840x2160, BT.2020/PQ,
limited range. The source contains Dolby Vision metadata; the native stats
surface reported DoVi peak/average metadata as well as HDR10 metadata.

Preserved rendering configuration
---------------------------------

The reference loaded /home/hewel/.config/mpv/mpv.conf. The relevant settings
and resolved values were:

* profile=gpu-hq (expands to high-quality).
* vo=gpu-next, gpu-api=vulkan, gpu-context=waylandvk, hwdec=vaapi.
* target-colorspace-hint=yes.
* tone-mapping=bt.2446a, tone-mapping-param=default,
  gamut-mapping-mode=auto, hdr-compute-peak=auto.
* hdr-peak-percentile=99.995003, hdr-contrast-recovery=0.3.
* scale=ewa_lanczossharp, cscale=ewa_lanczossharp, dscale=mitchell.
* scale-antiring=0.7, cscale-antiring=0.7.
* correct-downscaling=yes, linear-downscaling=yes, sigmoid-upscaling=yes.
* dither-depth=auto (actual target is 10-bit), dither=fruit.
* target-trc/target-prim/target-peak/target-contrast/hdr-reference-white=auto;
  use the resolved target above when there is no native swapchain to infer it.
* No ICC profile, automatic ICC profile disabled, no LUT/target LUT,
  no GLSL shaders.
* video-sync=audio, interpolation=no.
* wayland-disable-vsync=yes is configured; the installed executable reports
  it as a deprecated alias for wayland-internal-vsync=no.

Do not retune tone mapping, gamut mapping, contrast, or saturation by eye.
For matched frame comparison, disable scripts, OSC, subtitles and OSD on both
sides consistently, without changing these rendering options. Preserve the
reference's ten-bit target; an eight-bit iced surface is not equivalent.

Evidence and acceptance status
------------------------------

The initial native run rendered one frame and exited successfully. A second
paused native run was queried over JSON IPC. Local diagnostic evidence is in
/tmp/mpv-iced-baseline/native-properties.json,
/tmp/mpv-iced-baseline/native-options.json,
/tmp/mpv-iced-baseline/mpv.conf, and
/tmp/mpv-iced-baseline/native-solitude-30.png; detailed logs are
/tmp/mpv-native-baseline.log and /tmp/mpv-target-baseline.log.
Temporary paths are session evidence, not durable distributed assets.
The initial screenshot includes the user's statistics overlay and is not a
clean pixel-comparison reference. No copyrighted reference frames are added
to this repository.

The initial baseline capture alone did not establish GPU embedding, matched
picture comparison, sustained playback, or audio/video synchronization.
A source screenshot alone does not prove the final compositor
output. Both native and embedded surfaces must use the same physical size,
frame position, rendering settings, and output encoding for that comparison.

Host-stage verification
-----------------------

The fork built with Vulkan enabled against the versions above. Vulkan headers
were absent from /usr/include despite a working loader. The cached
vulkan-headers-1:1.4.357.0-1 package was extracted under
/tmp/mpv-vulkan-headers, without changing system packages. Build command::

    meson setup build -Dlibmpv=true -Dtests=true -Dbuildtype=debugoptimized \
        -Dvulkan=enabled -Dc_args=-I/tmp/mpv-vulkan-headers/usr/include
    meson compile -C build -j 16
    meson test -C build --print-errorlogs

All 36 existing tests passed on the initial host implementation. Independent
review subsequently identified a paused redraw feedback loop and a missing
capacity/resize notification. The dimension-change guard and
mpv_gpu_next_request_redraw resolve those issues. A real Vulkan host smoke
exercised an initially unavailable target, scheduler-driven recovery, stable
pause, paused resize from 961x541 to 481x271, resumed frames, and teardown:
17 acquired targets, 17 successful presentation releases, zero discards.
The revised code rebuilt successfully. These checks are host-stage evidence,
not iced presentation or audio/video acceptance.

The host currently falls back to software decoding (yuv420p10) with the same
hwdec=vaapi setting: the existing hardware interop expects a native Vulkan
context. gpu-next still performs rendering into the GPU image. Full-chain
zero-copy was not a requirement; hardware decoding must not be claimed here.

At 120 seconds and 961x541, installed mpv and fork native diagnostic screenshots
were byte-for-byte equal after decoding. However, screenshot-to-file window
uses a separate render path, not a copy of the native swapchain, and its inferred
transfer differs from the actual SDR surface. Comparing that screenshot directly
against host gamma2.2 output produced up to 7/255 differences. No color parameters
were changed in response.

A compositor capture of the actual fork native window, compared against the
host's RGB10A2 image converted to 8-bit solely for diagnostic comparison, had
maximum RGB difference 1/255, mean 0.191581/255 at that frame. This isolates and
supports the host's output encoding, but does not establish 10-bit precision
equivalence or iced acceptance. Use actual surface captures for final comparison.
The clean rendering configuration is TOOLS/gpu-next-host-baseline.conf; load
it explicitly on both paths with --no-config --include=<absolute path>.

iced GPU handoff
----------------

The isolated consumer is ../iced/examples/mpv_sdr_gpu. It uses winit and the
ordinary iced_wgpu renderer directly, so it can explicitly require a
Rgb10a2Unorm surface without changing iced's default compositor format policy.
It renders normal iced controls over the video; it does not replace any
JellyPilot production UI or business logic.

The public producer contract is include/mpv/gpu_next.h. Register the borrowed
host descriptor before mpv_initialize. iced creates the Vulkan device with an
exact retained enabled-feature chain, and mpv imports that device. The graphics
queue is family-specific index 0; both sides serialize queue operations with
the same mutex. Descriptor, feature nodes, device and callbacks outlive
mpv_terminate_destroy.

The consumer owns three raw Vulkan image slots. mpv borrows an idle slot and
renders through the existing gpu-next renderer. Successful release happens
only from mpv's scheduled flip_page, after producer GPU completion. This is
permission to present, not proof of compositor scanout. The consumer then makes
one GPU image copy into a private current-frame texture. A source slot becomes
available only after that copy completes. UI redraws may safely resample the
private texture; they never resample an already-returned producer image.

There is no playback-path CPU readback. Decoded software frames are uploaded by
mpv, and this prototype blocks for producer GPU completion; neither full-chain
zero-copy nor optimized cross-queue overlap is claimed. Backpressure is bounded.
After an exhausted pool recovers, or a paused window resizes, the application
thread calls mpv_gpu_next_request_redraw outside the shared queue lock.

wgpu 29 forbids mixing its tracked encoder API with raw HAL recording on one
encoder. The consumer uses three ordered command buffers in one submission:
tracked transition to COPY_DST, raw image copy, tracked transition to RESOURCE.
The destination is initialized through wgpu before any raw copy. Raw image
wrapping without initialization would risk wgpu discarding existing contents.

Both input and output preserve gamma2.2 nonlinear code values in ten-bit UNORM.
The shader does not perform Dolby Vision processing or tone mapping; those
remain in gpu-next/libplacebo. An sRGB attachment would require inverse-sRGB
before automatic attachment encoding, but this consumer requires ten-bit UNORM
and does not silently fall back to an eight-bit sRGB surface.

Run the isolated consumer from the iced fork::

    cargo run -p mpv_sdr_gpu -- /absolute/path/to/video.mkv \
        --libmpv /absolute/path/to/mpv/build/libmpv.so.2.5.0 \
        --baseline /absolute/path/to/mpv/TOOLS/gpu-next-host-baseline.conf \
        --start 120 --width 961 --height 541 --pause \
        --ipc /tmp/mpv-iced.sock

For clean comparison add --no-overlay. A tiling compositor can override the
requested size; verify video-target-params and the actual window capture size.
Do not compare a tiled 1871x2135 surface against a 961x541 reference.

Actual surface comparison
-------------------------

At 961x541 physical pixels, the three matched native/iced compositor captures
gave the following RGB code differences (8-bit screenshot representation):

=========== ============================= =========== ==============
Position    Examined content              Maximum     Mean
=========== ============================= =========== ==============
30 seconds  Bright sun, green foliage     1/255       0.187670/255
120 seconds Dark interior, shadow detail  1/255       0.195112/255
600 seconds Skin, sunlit fabric, shadows  1/255       0.184799/255
=========== ============================= =========== ==============

The images were also inspected visually. Normal iced checkbox overlay rendering
was verified on the same ten-bit video surface. These captures establish
agreement at screenshot precision, not full ten-bit numerical identity,
calibrated monitor accuracy, HDR output, or Dolby certification.

Sustained playback measurement
------------------------------

The embedded GPU consumer played the same source continuously for 120.15 seconds
using PipeWire audio. There were 120 one-second IPC samples. Video position
advanced 119.084 seconds over the sampled 119.150-second interval. Video frame
drops, decoder frame drops, and VO delayed-frame counts remained zero.

The largest sampled absolute mpv avsync was 25 ms at initial resume. After the
first two seconds, the maximum was 0.188 ms and mean was 0.008619 ms. An actual
window capture during playback confirmed the visible image had advanced.
These values measure mpv's audio/VO scheduling, not the downstream compositor's
scanout or a microphone-based end-to-end audiovisual delay. There is no native
presentation feedback in this host API; physical speaker/display lip-sync
acceptance remains distinct from this automated stability check.

Playback controls and user acceptance
-------------------------------------

After the matched surface comparison passed, the isolated iced example's
controls were connected to libmpv: pause/play, relative and exact absolute seek,
volume, playback clock/duration, loading/seeking/buffering/idle state, and
visible asynchronous command/playback errors. Eight observed properties drive
the displayed state; the UI does not advance a substitute clock. Event payloads
are copied before the next mpv_wait_event call, and commands/events are processed
on the application thread outside the shared GPU queue lock.

The final example builds successfully and passes package-scoped formatting and
``cargo clippy -p mpv_sdr_gpu --no-deps -- -D warnings``. A dependency-inclusive
clippy run is blocked by an existing ``default_constructed_unit_structs`` warning
at iced's wgpu/src/layer.rs:433 in the no-image feature configuration. That
unrelated renderer file was intentionally not changed.

The user then manually tested the prepared native and iced windows and reported
no difference from native mpv and that the iced controls were usable. This is
the human picture/control acceptance result, distinct from the numerical
surface and mpv scheduling measurements above. No additional color tuning was
performed. The manually prepared windows were paused at 600 seconds with
matching actual 1311x737 RGB10A2/BT.709/gamma2.2 targets before user interaction.

The raw-encoding panic found in the first consumer run was corrected before
these comparisons. Independent source review covered the corrected three-buffer
submission and subsequent control/event integration. Source review alone was
not treated as runtime proof.

Temporary C/Python verification probes and the unused input-helper extraction
were removed. Diagnostic images/JSON remain in /tmp/mpv-iced-baseline; the
temporary Vulkan headers remain because the configured local build needs them.
The two manual test windows are deliberately left open for the user. No commit,
push, system package installation, or production JellyPilot migration was made.
