Managed daemon data sockets require the connection-key attachment described in
[control.md](control.md) before this binary HELLO. The low-level
`rdb_server_serve_fd` assumes its host has already authorized the transport.

# Bootstrap RemoteDB v0 wire contract

This defines the implemented subset, not the complete product spec.
All integers are little endian; no native structures are sent. The 24-byte
header is magic RDB0 (4), version 0 (2), opcode (2), flags (4),
statement id (4), payload length (8). Payload lengths exceeding the negotiated
frame maximum are rejected before payload consumption or allocation.
Maximum defaults to 65536 and must be between 32 and 65536 inclusive.
Wire 64-bit values use two 32-bit limbs in the C89 API; no long long or
stdint.h is required. Total stream length is independent of frame length.

Requests have flags 0. Responses have flags 1 and echo opcode and statement.
Connection operations use statement 0; bootstrap statement operations use 1.
Unknown operations after HELLO are drained through bounded chunks and return
UNSUPPORTED. Bad framing, flags, lengths, and HELLO ordering close the session.
EOF at a frame boundary releases resources; a truncated frame is transport loss.

Every response begins with 16 fixed bytes: status (u32), requested parameter
ordinal (u32), and operation-specific value (u64). Status values are OK=0,
NEED_INPUT=1, NO_DATA=2, UNSUPPORTED=3, ERROR=4. Detailed diagnostics are pending.
Local client failures use the negative status values documented in the headers.
A transport error during EXECUTE or its input continuation is OUTCOME_UNKNOWN;
the dead session is never reused or retried automatically.

| Opcode | Name | Request | Response after the fixed prefix |
| --- | --- | --- | --- |
| 1 | HELLO | min version u16, max version u16, frame limit u32 | none; value is negotiated limit |
| 2 | CONNECT | empty | none; host-selected backend |
| 3 | DISCONNECT | empty | none; session ends |
| 16 | SQL_BEGIN | empty | none |
| 17 | SQL_DATA | chunk of unchanged SQL bytes | none |
| 18 | SQL_END | empty | none |
| 19 | EXECUTE | empty | none; NEED_INPUT ordinal or final output length |
| 32 | PARAM_DATA | chunk of current requested parameter | none |
| 33 | PARAM_END | empty | none; next NEED_INPUT or final output length |
| 48 | COLUMN_READ | desired count u32, at most limit minus 16 | at most 4096 bytes; NO_DATA is empty |
| 49 | RESET | empty | none |

Sequence: HELLO, CONNECT, SQL_BEGIN, SQL_DATA*, SQL_END, EXECUTE,
(PARAM_DATA*, PARAM_END)* as requested, COLUMN_READ* until NO_DATA,
RESET or DISCONNECT. No input bytes are sent before execution requests them.
This allows a future ODBC adapter to map SQLExecute/SQLParamData/SQLPutData
without whole-value staging. The backend may choose the parameter order.
Declared types, lengths, NULLs, and parameter binding are future additions;
the mock decides which opaque input streams to request.

The server acknowledges each chunk only after consumption. At most one request
is outstanding. A callback failure drains the remainder of that bounded frame,
poisons the statement, and requires RESET or DISCONNECT. RESET is valid while
input or output is pending. A connection loss invalidates all statement state.
