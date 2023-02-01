# Architecture

`blackspan` is organized as a decoder core with a thin command-line front end.
The core is a set of subsystems with distinct responsibilities that pass typed
data forward; no subsystem reaches back into the raw byte stream once framing is
done.

## Data flow

```
recording bytes
      |
      v
  [frame]     decode + CRC-verify the frame stream into typed records
      |
      v
  [schema]    register channel schemas; resolve sample-block layouts
      |
      v
  [session]   reconstruct session boundaries across checkpoints/reconnects
      |
      v
  [channel]   accumulate decoded samples into per-channel column stores
      |
      v
  [derive]    evaluate derived-channel expressions over recorded channels
      |
      v
  [anomaly]   correlate faults and state transitions into a timeline
      |
      v
  [report]    render deterministic text / JSON / Graphviz output
```

`workspace` is a cross-cutting subsystem: it provides content-addressed storage
for imported recordings and reconstructed sessions so that later commands can
replay a session without re-reading the original device file.

## Subsystems

- **frame** (`src/frame`) — wire framing: header/payload CRC verification,
  single-frame decode, and sequential stream decoding. Owns no domain state.
- **common** (`src/common`) — shared primitives: CRC, little-endian byte IO, and
  (as they are introduced) pooled storage and arena allocation used by the
  stateful subsystems.
- **schema** (`src/schema`) — the channel schema registry: schema definitions,
  versioning, and sample-layout resolution.
- **session** (`src/session`) — the recorder session state machine: begin/end,
  checkpoint rollover, and reconnect handling.
- **channel** (`src/channel`) — per-channel column stores for decoded samples,
  including compaction of retired sample ranges.
- **derive** (`src/derive`) — the derived-channel expression evaluator.
- **anomaly** (`src/anomaly`) — fault and state-transition correlation.
- **report** (`src/report`) — deterministic rendering to text, JSON, and dot.
- **workspace** (`src/workspace`) — content-addressed session storage and
  replay.

## Design notes

- Framing is a hard boundary: once a frame is decoded, downstream subsystems
  work only with typed records, never with raw offsets.
- Timestamps are device-monotonic; wall-clock interpretation, if any, is a
  report-time concern and never affects decoding.
- Errors are values: decoding surfaces a status rather than throwing, so the CLI
  can report exactly where a stream became unreadable.
