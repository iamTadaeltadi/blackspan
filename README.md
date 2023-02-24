# blackspan

`blackspan` is an offline decoder and session reconstructor for diagnostic
flight-recorder logs produced by embedded and industrial equipment. Devices
such as controllers, drives, and instrumentation packs continuously write a
compact binary event stream to on-board storage. When a unit is returned for
service, `blackspan` turns that raw stream back into a readable, queryable
picture of what the device was doing: its sessions, sample channels, state
transitions, configuration changes, and faults.

The tool is fully offline and hermetic. It needs no network service, no device
runtime, and no vendor toolchain. A recording is a self-contained file; the
decoder reconstructs everything from the framed byte stream alone.

## Why it exists

Field recorders are written under tight memory and time budgets, so the on-disk
format is terse: fixed frame headers, CRC-protected payloads, schema-defined
sample layouts, and monotonic device timestamps rather than wall-clock time.
Reading such a stream by hand is impractical. `blackspan` provides:

- deterministic decoding of the frame stream with CRC verification,
- reconstruction of session boundaries across recorder checkpoints and
  reconnects,
- schema-driven interpretation of sample blocks into typed channels,
- derived channels computed from recorded ones,
- fault and state-transition timelines, and
- stable text, JSON, and Graphviz reports for triage and archival.

## Building

`blackspan` builds with CMake and a C++17 compiler. It has no third-party
dependencies.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

An AddressSanitizer/UBSan build is available for development:

```sh
cmake -S . -B build-asan -DBLACKSPAN_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan
```

## Usage

```sh
# List the frames in a recording with type, device, sequence, and length.
blackspan frames recording.bsp

# Reconstructed recorder sessions and any ordering diagnostics.
blackspan sessions recording.bsp

# Registered channel schemas, and the per-channel sample column stores.
blackspan schemas recording.bsp
blackspan channels recording.bsp

# Fault and state-transition timeline with a severity breakdown.
blackspan faults recording.bsp

# Render the whole reconstructed model as text, JSON, or Graphviz.
blackspan report recording.bsp --format text|json|dot

# Query the model with a small triage language.
blackspan query recording.bsp "select samples from channel 1 where raw > 180"
blackspan query recording.bsp "select faults where severity >= error"

# Summary statistics for one channel field.
blackspan stats recording.bsp --channel 1 --field 0

# Export a channel to CSV, TSV, or newline-delimited JSON.
blackspan export recording.bsp --channel 1 --format csv|tsv|ndjson

# Store and inspect recordings in a content-addressed workspace.
blackspan import unitA recording.bsp --workspace ./ws
blackspan inspect --workspace ./ws
```

Every command reads the same recording format and runs the same decoder core;
see `docs/` for the frame grammar, session state model, and architecture.

## Format and documentation

- `docs/FRAME_FORMAT.md` — the binary frame grammar and record types.
- `docs/ARCHITECTURE.md` — subsystem boundaries and data flow.

## License

MIT. See `LICENSE`.
