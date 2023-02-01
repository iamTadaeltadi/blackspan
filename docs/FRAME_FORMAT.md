# Recorder Frame Format

A `blackspan` recording is a flat sequence of frames. Each frame is
self-delimiting: a fixed 28-byte header carries a payload length and a header
CRC, and the payload is followed by a 4-byte payload CRC. All multi-byte
integers are little-endian.

## Frame layout

```
offset  size  field         notes
------  ----  ------------  --------------------------------------------
0       4     magic         0x31505342 ("BSP1")
4       1     version       currently 1
5       1     record_type   see "Record types"
6       2     flags         record-type-specific bit flags
8       4     device_id     stable identifier of the recording device
12      4     session_seq   monotonic sequence within a recorder session
16      8     monotonic_ts  device monotonic clock, nanoseconds
24      2     payload_len   length of the payload in bytes (0..65535)
26      2     header_crc    CRC-16/CCITT-FALSE over bytes 0..25
28      N     payload       payload_len bytes
28+N    4     payload_crc   CRC-32 (ISO-HDLC) over the payload
```

Total frame size is `28 + payload_len + 4`.

### CRCs

- `header_crc` is CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no
  reflection, no final xor) computed over the 26 header bytes that precede it.
- `payload_crc` is CRC-32/ISO-HDLC (reflected poly `0xEDB88320`, init and final
  xor `0xFFFFFFFF`) computed over the payload bytes.

A frame whose header CRC does not match is rejected before its payload length
is trusted. A frame whose payload CRC does not match is reported as corrupt and
decoding of the stream stops at that offset.

## Record types

| value | name              | meaning                                            |
|------:|-------------------|----------------------------------------------------|
| 0x01  | session-begin     | opens a session; carries device metadata           |
| 0x02  | session-end       | closes the current session                         |
| 0x03  | schema-def        | defines a channel schema used by later samples     |
| 0x04  | sample-block      | a batch of samples for one channel                 |
| 0x05  | state-transition  | a device state-machine transition                  |
| 0x06  | config-change     | a configuration parameter change                   |
| 0x07  | fault             | a fault or anomaly report                          |
| 0x08  | marker            | operator marker / bookmark                         |
| 0x09  | checkpoint        | recorder checkpoint / storage rollover             |
| 0x0A  | reconnect         | recorder resumed after a gap                       |
| 0x0B  | derived-def       | defines a derived channel expression               |
| 0x0C  | export-request    | in-band request to render a report                 |

Payload grammars for each record type are defined by the subsystem that owns
them and are documented alongside those subsystems as they are implemented.

## Stream rules

- Decoding is sequential from offset 0.
- A trailing partial frame at end-of-stream is reported as `incomplete`, not an
  error: recorders may be interrupted mid-write.
- `magic` re-synchronization is intentionally not performed; a corrupt frame is
  a hard stop so that no misaligned bytes are silently reinterpreted.
