# Recording script format (`.bsr`)

`blackspan assemble` turns a human-readable recording script into the exact
bytes a device recorder would have written. It is the inverse of the decode
pipeline: the schemas, sessions, channels, faults, and reports that the decoder
reconstructs all originate from the frames a script emits. Scripts are used for
regression fixtures, for reproducing a field failure that was reported as a
description rather than a file, and for exercising the decoder against a stream
of a known shape.

```sh
blackspan assemble script.bsr --out recording.bsp
blackspan assemble script.bsr            # writes the bytes to stdout
```

Assembly never stops at the first problem. Every malformed directive is reported
as a diagnostic carrying its line number, and the remaining lines are still
processed, so one run reports every error in a script. A script that produces any
diagnostic yields no bytes and a non-zero exit status.

## Lines

A script is line-oriented. Line endings may be LF or CRLF. A blank line, or a
line whose first non-blank character is `#`, is ignored. Every other line is a
directive: a name followed by whitespace-separated arguments.

An argument is either positional (a bare value) or a `key=value` pair. A value
may be bare or double-quoted; quote a value to include spaces or a leading `#`.
Inside a quoted value the escapes `\\ \" \n \r \t \0 \xHH` are recognized. A
trailing `#` outside quotes starts a comment.

```text
fault code=0x21 severity=error detail="over temp"   # a fault record
```

Numbers accept decimal, `0x` hex, `0b` binary, and `_` digit grouping. Signed
fields accept a leading `-`. Real fields accept the usual floating-point forms
plus the bare words `nan`, `inf`, and `-inf`.

## Stream state

These directives set the frame-header fields that following records inherit, so
a script does not repeat them on every line. Each takes its value as either the
first positional argument or a named key.

| Directive        | Sets                                                        |
|------------------|-------------------------------------------------------------|
| `device <id>`    | the device id (`id=`)                                        |
| `session <seq>`  | the running session sequence (`seq=`)                        |
| `ts <value>`     | the running monotonic timestamp (`value=`)                   |
| `advance by=<n>` | adds `n` to the running timestamp                            |
| `tick by=<n>`    | the amount added to the timestamp after each emitted frame   |
| `flags <value>`  | the header flags word (`value=`)                             |

After each emitted frame the running `session` sequence is incremented by one and
the running `ts` is advanced by the current `tick`. Any record directive may
override a single field for just that frame with `dev=`, `seq=`, `ts=`, or
`flags=`; the override does not move the running counters.

## Records

Each of these emits exactly one frame. Required arguments are marked `*`.

- `session-begin` — `class=` `fw=<maj>.<min>.<patch>` `epoch=` `label=`
- `session-end` — `reason=`
- `schema` — `id=`\* `name=` `fields=<name>:<type>,...`\* — declares a channel
  layout. Scalar types are `u8 i8 u16 i16 u32 i32 u64 i64 f32 f64`. The first
  layout for a channel is version 1; re-declaring an identical layout reuses it;
  a changed layout appends a new version.
- `samples` … `row` … `end` — a sample block. `samples channel=`\* `version=`
  `delta=` `blockflags=` opens the block against a declared schema (defaulting to
  its latest version); each `row <v0> <v1> ...` supplies one value per schema
  field; `end` emits the frame. Values are range-checked against their field
  type.
- `state` — `from=`\* `to=`\* `cause=` `reserved=`
- `config` — `param=`\* `old=` `new=`
- `fault` — `code=`\* `severity=<info|warning|error|critical|0..3>` `subsystem=`
  `detail=`
- `marker` — `kind=` `text=`
- `checkpoint` — `id=` `bytes=`
- `reconnect` — `gap=` `prev=`
- `derived` — `id=`\* `primary=`\* `name=` `expr=` — a derived channel whose
  samples are a formula over recorded channels.
- `export` — `format=<text|json|dot>` `sections=`
- `raw` — `type=`\* `hex=` `text=` — an arbitrary record type with a payload
  given as hex byte pairs and/or literal text. Useful for forward-compatibility
  and malformed-payload fixtures.

## Damage

The `damage` directive perturbs the frame most recently emitted, so a script can
produce the corrupt recordings a decoder must survive. `damage mode=<mode>`, or
`damage <mode>`, where mode is one of:

| Mode          | Effect                                                       |
|---------------|-------------------------------------------------------------|
| `header-crc`  | corrupts the header CRC field                                |
| `payload-crc` | corrupts the payload CRC trailer                             |
| `magic`       | corrupts the frame magic                                     |
| `version`     | declares an unsupported format version (header CRC resealed) |
| `length`      | overstates the declared payload length (header CRC resealed) |
| `truncate`    | cuts `bytes=<n>` bytes off the end of the frame              |

The `version` and `length` modes reseal the header CRC so the decoder reaches the
check the mode targets rather than stopping at a CRC mismatch first; the other
modes leave the frame inconsistent on purpose.

## Example

```text
device 0x00A10001
session 1
ts 1000000
tick 1000

session-begin class=17 fw=2.4.11 epoch=3 label="drive-a1 bench run"
schema id=1 name=motor fields=rpm:u16,current_ma:i16,temp_c:i8
state from=0 to=1 cause=1
samples channel=1 delta=0
  row 1200 850 41
  row 1210 860 41
end
fault code=0x0104 severity=warning subsystem=2 detail="current limit approached"
session-end reason=0
```

See `examples/nominal.bsr`, `examples/fleet.bsr`, and `examples/degraded.bsr` for
complete recordings covering a healthy run, an interleaved multi-device
download, and a recording pulled off a unit that failed in service.
