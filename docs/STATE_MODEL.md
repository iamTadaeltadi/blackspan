# Recorder Session State Model

A recording is a flat frame stream, but the frames describe a stateful process:
a device opens a session, records activity, and closes the session, possibly
across storage checkpoints and reconnects. The `session` subsystem reconstructs
that lifecycle and reports every place the stream departs from it.

Sessions are tracked **per `device_id`**. A single recording may interleave
frames from more than one device; each device advances through the lifecycle
independently.

## Lifecycle

```
        session-begin              session-end
  Idle ----------------> Active ----------------> (session recorded, closed=true)
   ^                      |  ^
   |     session-begin    |  | checkpoint / reconnect / marker / other records
   | (auto-close previous)|  | attach to the active session
   +----------------------+  +
```

| State  | Meaning                                                            |
|--------|-------------------------------------------------------------------|
| Idle   | no session is open for the device                                 |
| Active | a session-begin has been seen and not yet closed                  |

A session that reaches `session-end` is **closed** (`closed = true`) and its
`duration_ns()` is `end_ts - begin_ts`. A session that ends any other way
(a new begin arrives while active, or the stream ends) is recorded but left
`closed = false` with its end position set to the last record observed, and its
duration reported as zero. This keeps a truncated recording usable without
inventing an end time the device never wrote.

## Records within a session

- **session-begin** opens a session and carries device class, firmware version,
  schema epoch, and a label.
- **checkpoint** and **reconnect** attach to the active session. A reconnect
  records the gap and the previous sequence number so a resumed recording reads
  as one session rather than two.
- **marker** attaches an operator bookmark.
- Records owned by other subsystems (schema-def, sample-block, faults, …) do not
  change session state but are counted, and their sequence and timestamp still
  participate in the monotonicity checks below.

## Monotonicity

Within an active session both `session_seq` and `monotonic_ts` are expected to be
non-decreasing. A decrease is not fatal — the record is kept — but it is
reported as a `SeqRegression` or `TsRegression` diagnostic and counted on the
session, because it usually indicates a corrupted or spliced recording.

## Diagnostics

Reconstruction never throws and never discards data on a malformed order; it
records a diagnostic and continues. The diagnostic codes are:

| Code                    | Condition                                            |
|-------------------------|------------------------------------------------------|
| EndWithoutBegin         | a session-end arrived while Idle                     |
| BeginWhileActive        | a session-begin arrived while already Active         |
| ReconnectWhileIdle      | a reconnect arrived while Idle                       |
| CheckpointWhileIdle     | a checkpoint arrived while Idle                      |
| MarkerWhileIdle         | a marker arrived while Idle                          |
| SeqRegression           | session_seq decreased within a session               |
| TsRegression            | monotonic_ts decreased within a session              |
| UnterminatedSession     | the stream ended with a session still Active         |
| UnknownRecordInSession  | a record with an unrecognized type appeared          |
| TruncatedPayload        | a session record's payload ended early               |

## Reconstruction guarantees

- The set of reconstructed sessions and the diagnostic list are a pure function
  of the frame stream: the same bytes always reconstruct to the same result.
- A truncated final frame in the stream is surfaced by the frame layer as
  `incomplete` and does not, by itself, produce a session diagnostic.
- `session_index` is assigned per device in arrival order starting at zero.
