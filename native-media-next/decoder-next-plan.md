# Android `AMediaCodec` bridge plan

Status update 72dp: real G711 decoding is implemented and device-tested; see [current evidence and limitations](README.md). This document preserves the earlier design, not a claim that flush/restart/video or the entire cancellation matrix is complete. The installed SDK also provides API20 synchronous Query/Get buffer APIs; callback bridging is an implementation choice, not the only OH native API model.

Date: 2026-11-09  
Scope: implementation plan from the frozen 54-import inventory and the installed DevEco SDK headers. This is not a claim that a decoder has been implemented or exercised on the device.

## ABI surface to cover

The frozen `unity-undefined-symbols.json` lists these 14 codec imports (entries at lines 1393–1458):

| Android NDK call | Synchronous contract the shim must preserve |
|---|---|
| `AMediaCodec_createDecoderByType(mime)` | Create a decoder object for a MIME type. Choose the audio or video OH decoder family from the MIME prefix; fail unsupported if creation fails. |
| `AMediaCodec_configure(codec, format, surface, crypto, flags)` | Configure from translated track metadata. Do not claim crypto support unless a real OH key-session bridge exists. Video surface is a separate capability (below). |
| `AMediaCodec_start(codec)` | Start after configure. OH begins notifying input availability asynchronously. |
| `AMediaCodec_stop(codec)` | Cancel blocked dequeue calls, transition state, then stop OH outside queue/registry locks. |
| `AMediaCodec_flush(codec)` | Invalidate all Android-visible input/output indices and queued events, cancel waiters, flush OH, then accept only fresh callback indices. |
| `AMediaCodec_delete(codec)` | Wake waiters, stop/destroy OH exactly once, drain in-flight callbacks, then release callback state and registry slot. |
| `AMediaCodec_dequeueInputBuffer(codec, timeoutUs)` | Wait for an available OH input callback ticket; return its index, `INFO_TRY_AGAIN_LATER` on timeout, and a real error on stop/failure. |
| `AMediaCodec_getInputBuffer(codec, index, out_size)` | Return the OH input memory address and capacity for a dequeued, still-owned index. Pointer stays valid only until that index is queued back to OH. |
| `AMediaCodec_queueInputBuffer(codec, index, offset, size, ptsUs, flags)` | Validate bounds and that the index is currently application-owned; set OH buffer attributes and submit it. The index/buffer become inaccessible until OH reports that input index again. |
| `AMediaCodec_dequeueOutputBuffer(codec, info, timeoutUs)` | Wait for output or a queued format event; return output index, `INFO_OUTPUT_FORMAT_CHANGED`, `INFO_OUTPUT_BUFFERS_CHANGED` only when justified, `INFO_TRY_AGAIN_LATER`, or an error. |
| `AMediaCodec_getOutputBuffer(codec, index, out_size)` | Return the output buffer address/capacity while retaining the OH output index. It remains valid until release. |
| `AMediaCodec_releaseOutputBuffer(codec, index, render)` | Release the held OH output index exactly once. Audio has no render-to-surface behavior; video render requires a real OH native window. |
| `AMediaCodec_getOutputFormat(codec)` | Return a caller-owned Android format wrapper based on `OH_*Decoder_GetOutputDescription`; destroy the OH format after wrapping/copying. |
| `AMediaCodec_setOutputSurface(codec, surface)` | Requires a real Android-`ANativeWindow` to OH-`OHNativeWindow` unwrap/ownership bridge. Return unsupported until that bridge exists; never cast the opaque Android pointer. |

The usual Android dequeue special values are `INFO_TRY_AGAIN_LATER = -1`, `INFO_OUTPUT_FORMAT_CHANGED = -2`, and `INFO_OUTPUT_BUFFERS_CHANGED = -3`. Keep these distinct from `AMediaCodec` errors and real nonnegative buffer indexes. For output, copy each callback's `OH_AVCodecBufferAttr` into the event record; the callback-provided attribute pointer is only valid during the callback.

## OH callback and buffer contract

The SDK provides callback-driven codec APIs. The preferred modern signatures are declared in:

- `native_avcodec_base.h:78,96,140,152,185–190` — `OH_AVCodecOnError(codec, errorCode, userData)`, `OH_AVCodecOnStreamChanged(codec, format, userData)`, `OH_AVCodecOnNeedInputBuffer(codec, index, OH_AVBuffer*, userData)`, `OH_AVCodecOnNewOutputBuffer(codec, index, OH_AVBuffer*, userData)`, gathered in `OH_AVCodecCallback`.
- `native_avcodec_audiocodec.h:64,103,121,136,151,166,182,208,256,274` — current audio create/register/configure/prepare/start/stop/flush/description/push/free entry points. `OH_AudioCodec_CreateByMime(mime, false)` selects a decoder. The current buffer APIs are `OH_AudioCodec_PushInputBuffer(codec,index)` and `OH_AudioCodec_FreeOutputBuffer(codec,index)`.
- `native_avcodec_videodecoder.h:59,104,121,142,182,199,216,233,250,277,393,414,476` — video creation, current callback registration, surface, configure/lifecycle/description, and indexed input/output operations. Video currently uses the `OH_VideoDecoder_*` family in this SDK.
- `native_avbuffer.h:84–96,145–166` — buffer attributes/address/capacity. The indexed input/output callback buffers are OH-owned; the client must not destroy them.
- `native_avbuffer_info.h:52–84` — flags, including `AVCODEC_BUFFER_FLAGS_EOS`, `SYNC_FRAME`, and `CODEC_DATA`.

There are older API 9 callback forms too: `OH_AVCodecAsyncCallback` in `native_avcodec_base.h:167–172` passes `OH_AVMemory*` and an output `OH_AVCodecBufferAttr*`; `OH_AudioDecoder_SetCallback` / `OH_VideoDecoder_SetCallback` and `PushInputData` / `FreeOutputData` use those forms. They are deprecated since API 11 in favor of `OH_AVCodecCallback` and `OH_AVBuffer` calls. Prefer the modern callback APIs to avoid copying between OH memory and OH buffers. If an older target requires the legacy forms, copy the output attributes synchronously inside the callback and use `OH_AVMemory_GetAddr/GetCapacity` for the legacy memory objects.

The OH output-buffer contract ties index ownership to release. Video's header states that an input buffer cannot be accessed after push until its index is reported again, and output remains owned until `FreeOutputBuffer` or render; audio has equivalent indexed push/free calls. Therefore the Android wrapper must retain an index-to-buffer map from callback until `queueInputBuffer` / `releaseOutputBuffer`, and reject stale, duplicate, or foreign indexes.

## Bridge state and deadlock rules

Use a small Android codec state behind a tagged, generation-checked registry token, as with the extractor/format wrappers. Each state owns the OH codec pointer, codec kind, lifecycle state, callback context, an instance queue mutex/condition variable, available-input tickets, queued-output/format events, and maps of indexes currently owned by the Android caller.

The global registry lock is only for validating and pinning/unpinning the token. Never hold it while calling an OH API, waiting on a condition variable, or entering a callback. Otherwise OH may synchronously trigger a callback which needs the registry and deadlocks. Callback `userData` should point to a stable per-instance callback context (or a validated token plus a separate lifetime pin); callbacks should use only the instance queue lock, enqueue a compact event, signal waiters, and return. They must not call decoder APIs while holding the queue lock.

Likewise, do not hold the instance queue mutex across `OH_*Start/Stop/Flush/Destroy/Push/Free` calls. Change state under lock, drop it, call OH, then reacquire to publish success/failure. If two lifecycle operations need serialization, use a separate per-instance operation mutex which callbacks never acquire. Callback state must remain allocated until `Destroy` has completed and all callbacks already in flight have exited; use a callback-in-flight counter/condition or equivalent lifetime pin. Do not free `userData` merely because the Android token was removed.

Input flow:

1. `onNeedInputBuffer` enqueues `(index, OH_AVBuffer*)` as available and signals.
2. `dequeueInputBuffer` removes one ticket and marks it app-owned. `getInputBuffer` exposes its `OH_AVBuffer_GetAddr` / `GetCapacity` without transferring ownership.
3. `queueInputBuffer` validates `offset + size <= capacity`, sets `pts`, `size`, `offset`, and translated flags, then calls the audio/video `PushInputBuffer`. On success, remove the app-owned pointer and do not touch it again until OH callbacks with that index.

Output flow:

1. `onNewOutputBuffer` reads/copies attributes as applicable, queues `(index, buffer, attr)`, and signals. For modern OH_AVBuffer callbacks, read attributes/address/capacity while the index is retained; bounds-check `offset` and `size`.
2. `dequeueOutputBuffer` returns the next queued event in order. On a format callback, queue a format-change event before later outputs so Android receives `INFO_OUTPUT_FORMAT_CHANGED` at the right point. The format callback's OH format is callback-lifetime only; snapshot/translate it immediately or use `GetOutputDescription` when the Android caller asks for the format.
3. `getOutputBuffer` returns the retained pointer/capacity. `releaseOutputBuffer` verifies app ownership and invokes audio free, or video free/render, then invalidates the Android-visible index.

Translate Android `BUFFER_FLAG_END_OF_STREAM`, sync-frame, and codec-config bits explicitly to/from OH buffer flags. An OH EOS output event is still an output index which must be released; do not turn callback errors into EOS. The callback's error path stores the actual OH error and wakes every waiter.

## Waits, timeout, flush, stop, and delete

Android `timeoutUs` is in microseconds. For positive values, compute a monotonic absolute `pthread_cond_timedwait` deadline with overflow-safe `timeoutUs * 1000`; zero is a poll, and a negative timeout waits indefinitely. Always loop on the queue predicate to handle spurious wakeups. A timeout yields `INFO_TRY_AGAIN_LATER`, not a codec failure.

Before flush, set a flushing state, increment the queue epoch, clear queues and all Android-owned indexes, and broadcast to wake blocked dequeues. Call OH flush without either mutex held; OH documents that Flush invalidates all callback indexes previously reported. Then reopen input/output dequeue for fresh callback events. Callbacks arriving while the flush transition is active should be dropped as stale. Stop and delete similarly publish a stopping/closing state and broadcast before OH stop/destroy. Waiters return a lifecycle error; they must not remain blocked until their original timeout. Make delete idempotence explicit at the registry boundary and invoke OH destroy exactly once.

`AMediaCodec_queueInputBuffer` flags/EOS must preserve the last input; after EOS is submitted, reject further input until flush/reconfigure. Output EOS is identified by the actual OH flag, and the returned output buffer follows normal release ownership.

## Audio/video capability split and surface

For audio MIME types, use `OH_AudioCodec_*` (available since API 11) and configure the OH format with translated Android media keys/values. For video MIME types, use `OH_VideoDecoder_*`; surface must be configured with `OH_VideoDecoder_SetSurface(codec, OHNativeWindow*)` before prepare when using surface mode. The SDK exposes no direct equivalence between Android `ANativeWindow*` and OH `OHNativeWindow*`. A real bridge must unwrap the Android handle produced by the app/window compatibility layer, acquire/reference the underlying OH window with the documented lifecycle, then call SetSurface. If the Android window was built through the EGL/window shim, unwrap that actual native-window backing object at its owning adapter; EGL and `ANativeWindow` are not interchangeable handles either. A pointer cast can pass an unrelated Android shim object into OH and is invalid. Until a window unwrap exists, video byte-buffer output can be supported separately, but `configure` with a surface and `setOutputSurface` must return unsupported.

`AMediaCrypto*` also cannot be passed as an OH object. Secure/DRM decode remains unsupported until the Android crypto/session API is bridged to the OH media-key APIs. Do not claim the whole codec API based on clear, byte-buffer decode.

## Smallest verifiable audio slice

1. Implement decoder creation for one audio MIME and configure from the extractor's real audio track format. A good first *compressed decode* validation is a tiny known AAC-LC sample/track: feed extractor AAC samples, queue EOS, and verify at least one PCM output buffer with plausible size/PTS before EOS. This exercises input ownership, callback wakeups, output ownership/release, EOS, and format translation.
2. As a quicker callback/queue smoke path, the already verified 48 kHz stereo PCM16 WAV has MIME `audio/raw`; the installed SDK declares `OH_AVCODEC_MIMETYPE_AUDIO_RAW` since API 18 (`native_avcodec_base.h:396`). Probe whether the target exposes a usable raw-audio codec before relying on it. That path validates buffer handoff but is a pass-through/raw path, not evidence that compressed decoding works.
3. Start/stop/flush/delete while a dequeue is blocked, verify the wait wakes, then repeat with dequeue timeout 0 and a finite microsecond timeout. Verify a stream-change callback maps to one `INFO_OUTPUT_FORMAT_CHANGED` event and that no output index is reused before release.

Keep the first implementation audio-only if schedule requires it, and return unsupported for video-only calls until the video callback/output and native-window requirements are satisfied. The direct minimum vertical slice is one real MIME, one configured format, asynchronous callback queues, Android dequeue/get/queue/release semantics, EOS, and correct teardown—not success stubs for all imports.

## Remaining evidence limits

The 54-import list proves relocation needs, not that every codec call is reached. The extractor now has a reported device WAV FD-to-samples/seek loop, but this does not prove codec decode, supported MIME availability, correct callback thread behavior, or a surface path. Confirm the target device's decoder capability and run the blocked-wait/flush/delete races on-device before treating the bridge as complete.
