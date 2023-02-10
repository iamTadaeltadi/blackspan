# Operations

`blackspan` is designed to be run offline against recordings pulled from a
device. This document describes how recordings are stored, how results stay
reproducible, and how the project is built and verified.

## Offline and hermetic

The tool performs no network access and has no runtime dependency on a device or
vendor service. A recording is a self-contained file; every result is derived
from its bytes. The build downloads nothing.

## Workspace and content addressing

The `workspace` subsystem provides a content-addressed store so that recordings
and their reconstructed views can be kept, deduplicated, and re-opened without
re-reading the original device file.

- Each stored object is identified by the SHA-256 of its bytes, rendered as a
  lowercase hex string. Identical content always yields the same identifier and
  is stored once, even if imported under two different names.
- A workspace catalog maps a human-facing recording name to its content id and
  metadata: byte size, total frame count, whether the recording terminated
  cleanly, and per-record-type counts.
- The file backend stores objects under `objects/<first-two-hex>/<rest>` and
  writes every object and the catalog atomically (to a temporary file, then
  rename) so an interrupted import cannot leave a half-written object. A
  workspace re-opened over the same root recovers its catalog and objects.
- An in-memory backend provides the same interface for transient use and tests.

## Reproducibility

- Decoding is deterministic. The frame stream, the reconstructed sessions, the
  channel series, and the rendered reports are pure functions of the input.
- Content identifiers are stable across machines because SHA-256 is
  content-only; no timestamps or paths enter the digest.
- Reports are emitted in a stable order with explicit escaping so that a
  recording renders byte-for-byte identically on repeated runs.

## Recovery and partial recordings

- A recording interrupted mid-write ends in a partial frame. The frame layer
  reports this as `incomplete` and stops cleanly; all complete frames before it
  are fully usable.
- A frame whose payload CRC fails is a hard stop at that offset: the decoder
  does not attempt to resynchronize, so no misaligned bytes are reinterpreted.
- Misordered lifecycle records (for example a session-end with no matching
  begin) never abort a run; they are surfaced as diagnostics and the rest of the
  recording is still processed.

## Building and verifying

```sh
# Standard build and tests.
cmake -S . -B build
cmake --build build
ctest --test-dir build

# AddressSanitizer / UBSan build used during development and in CI.
cmake -S . -B build-asan -DBLACKSPAN_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan
```

Continuous integration runs both configurations. The sanitizer job additionally
enables leak detection, which is available on the Linux CI runners.
