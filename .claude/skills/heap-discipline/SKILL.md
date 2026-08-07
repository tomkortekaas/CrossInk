---
name: heap-discipline
description: Memory and subsystem lifecycle discipline for the ESP32-C3 (~380KB RAM, no PSRAM, single 48KB framebuffer). Use whenever code allocates memory or adds a long-lived service such as BLE, Wi-Fi, a server, cache, buffer, or activity-owned state; also use for OOM, bad_alloc, blank rendering, or coexistence regressions. Covers safe allocation, fragmentation, runtime heap gates, teardown, and cross-feature validation.
---

# Heap Discipline (ESP32-C3)

CLAUDE.md states the allocation rules. This is the procedure you run while
writing the code and the gate you run before handing it back.

The constraint that makes every call matter: ~380KB RAM, no PSRAM, one 48KB
framebuffer. **Fragmentation, not total usage, is what kills this device.**
Free-heap can read fine while the largest free block is too small for the next
allocation. Optimize for not leaving holes, not just for using fewer bytes.

## Allocation decision procedure

Ask in order; stop at the first yes.

1. **Stack?** Local, bounded, under ~256 bytes total: plain array/struct. No
   heap, no fragmentation. Keep frames lean; the task stack is small.
2. **Compile-time constant?** `static constexpr` lives in flash, costs zero DRAM.
3. **Allocated once and reused for an activity's lifetime?** Allocate in
   `onEnter`, hold in a member, release in `onExit`. Never per-frame, never
   per-iteration.
4. **Dynamic and fallible?** `makeUniqueNoThrow<T>(...)` /
   `makeUniqueNoThrow<T[]>(n)` from `lib/Memory/Memory.h`. Null-check, `LOG_ERR`
   with the size, return false. It frees on every exit path.
5. **A C/SDK API takes ownership and frees it itself?** Only then raw
   `new (std::nothrow)` / `malloc`, with a comment naming who frees it.

Bare `new` / `new[]` is never correct here: under `-fno-exceptions` it calls
`abort()` on OOM instead of returning null.

## Fragmentation rules

- `std::vector`: `reserve(n)` before any `push_back` loop. Each growth is
  alloc-copy-free (three heap ops) and leaves a hole. Unknown n: estimate high.
- No repeated `new`/`delete` or growing containers inside a loop or render path.
  Hoist the allocation out of the loop.
- Large contiguous blocks fragment worst. Full-screen-class buffers use the
  chunked `storeBwBuffer` / `restoreBwBuffer` path in `GfxRenderer` so they
  never demand one contiguous 48KB block. Reuse that path. Do not malloc a
  second full-screen buffer.
- `std::string` / Arduino `String`: acceptable on cold paths (file I/O, one-shot
  setup). Banned on hot/render paths. Build text with a stack `char[]` +
  `snprintf`; if a `std::string` is unavoidable, `reserve` it first.

## Justify every allocation

Per CLAUDE.md's evidence rule: when you add a heap allocation, state in one line
why stack/static/reuse was rejected and the worst-case size. If you cannot name
the size, you cannot budget it, and you should not allocate it.

## Runtime memory gate

PlatformIO's RAM percentage describes static usage, not runtime headroom. A
successful build and isolated feature test are therefore not release evidence.

For a new subsystem or a change that coexists with the reader, log on target:

- free 8-bit heap and largest free 8-bit block after boot, after subsystem
  start, at peak work, after stop/deinit, and during the heaviest existing flow;
- minimum free heap across the run;
- the largest requested allocation and an explicit safety margin.

Use `heap_caps_get_free_size(MALLOC_CAP_8BIT)` and
`heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)`. The gate is whether the
largest block can satisfy the known worst allocation with margin, not whether
some heap remains. If the worst allocation is unknown, instrument it first.

Repeat start/work/stop cycles. Free heap and largest block must recover to a
stable range; a downward trend is fragmentation or incomplete teardown.

## Subsystem lifecycle and coexistence

- Prefer a bounded receive window for memory-heavy transports. After success,
  timeout, malformed input, disconnect, and cancellation, stop tasks and timers,
  unregister callbacks, release buffers, and fully deinitialize the stack.
- Validate the service together with the worst existing consumers, not only in
  isolation: image-heavy EPUB/rendering, settings rebuild, file transfer/Wi-Fi,
  OPDS/sync, dictionaries, cover generation, sleep, and wake.
- Persist validated, size-bounded state before transport teardown. Rendering a
  dashboard or sleep screen must not require a live BLE stack or retain a view
  into transport-owned memory. Persistence needs version/length/CRC validation
  and an atomic fallback to the last valid snapshot.
- A feature that works but makes an existing flow crash or render blank is a
  failed memory test. Disable or shorten its lifetime until coexistence passes.

## Self-review before handoff

- [ ] No bare `new`/`new[]`. Every fallible alloc is `makeUniqueNoThrow`, or a
      raw alloc with an explicit owner comment.
- [ ] Every allocation is null-checked with `LOG_ERR` before the error return.
- [ ] No allocation inside a loop or render path that could be hoisted.
- [ ] Every `push_back` loop has a preceding `reserve`.
- [ ] Anything allocated in `onEnter` is released in `onExit`; member `HalFile`
      closed there too.
- [ ] No second full-screen buffer; grayscale uses store/restoreBwBuffer.
- [ ] Each new allocation carries a one-line size + why-not-stack/static note.
- [ ] Target logs prove free heap, minimum free heap, and largest free block at
      lifecycle boundaries and during the heaviest existing flow.
- [ ] Repeated start/stop plus success, timeout, malformed-input, disconnect,
      and cancellation paths show stable recovery after full deinitialization.
- [ ] Persisted state has bounded ownership and remains usable after its
      transport is gone; sleep/wake and reader coexistence are tested.
- [ ] Static RAM percentage, compile success, and an isolated happy path are not
      used as substitutes for the runtime gate.
