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

# One merged, time-ordered view of every event across all subsystems.
blackspan timeline recording.bsp --kinds fault,state-transition --device 0x11

# Structural lint over the reconstructed model (exit 2=errors, 1=warnings).
blackspan validate recording.bsp

# Compare two recordings structurally (a golden run against a field return).
blackspan diff baseline.bsp comparand.bsp

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

# Assemble a recording from a readable script (the inverse of decoding).
blackspan assemble script.bsr --out recording.bsp
```

Every command reads the same recording format and runs the same decoder core;
see `docs/` for the frame grammar, session state model, and architecture.

## Authoring recordings

Most of the time a recording comes off a device, but for regression fixtures,
for reproducing a failure reported as a description, and for exercising the
decoder against a stream of a known shape, `blackspan assemble` builds a
recording from a line-oriented script. Stream-state directives (`device`,
`session`, `ts`, `tick`, `flags`) set the frame-header fields that following
records inherit; record directives (`session-begin`, `schema`, `samples`/`row`/
`end`, `state`, `fault`, `marker`, and the rest) each emit one frame; and the
`damage` directive perturbs the most recent frame so a script can produce the
corrupt recordings a decoder must survive. The grammar is documented in
`docs/SCRIPT_FORMAT.md`, and the worked examples live under `examples/`:

```sh
blackspan assemble examples/nominal.bsr --out nominal.bsp
blackspan report nominal.bsp --format text
```

The `examples/check.sh` script regenerates every example from its `.bsr` source
and diffs the decoder output against the checked-in transcripts in
`examples/expected/`, so the wire format, the decoder, and the documented output
stay in step.

## Format and documentation

- `docs/FRAME_FORMAT.md` — the binary frame grammar and record types.
- `docs/ARCHITECTURE.md` — subsystem boundaries and data flow.

## License

MIT. See `LICENSE`.
