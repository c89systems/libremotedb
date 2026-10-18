# Managed control and connection contract

The required control socket is separate from the binary database socket. Both
use AF_UNIX SOCK_STREAM. Pathnames are default; Linux abstract names are explicit
and include no filesystem path or leading `@` in the API. The implementation
adds the initial NUL and uses the exact address length without a trailing NUL.
Darwin rejects abstract endpoints.

## Control exchange

One request per accepted connection. The client streams one JSON object then
half-closes its write side. lonejson parses the object and confirms EOF before
applying any operation. Replies are streamed JSONL objects: zero or more records,
followed by exactly one `type: "done"` record with status `ok`, `error` or `unknown`.
The C API invokes a callback per record and retains no complete response.
Callback cancellation closes the transport. No SQL, parameter or BLOB fields
exist on this channel. Requests are capped at 16 KiB including unknown fields.
The parent polls nonblocking sockets and retains resumable lonejson state and
one output chunk per control exchange. Each control exchange (including reply
output) and incomplete key handshake has a five-second monotonic deadline from
acceptance; partial progress never extends it. A valid key hands the transport
to an isolated worker; backend initialization and its acknowledgement occur there,
outside the parent's handshake deadline. Expiry closes the transport. There are separate
pools of 16 control exchanges and 64 key handshakes; a saturated pool closes new
peers without consuming the other pool. Established backend workers have their
own transports and are unaffected by those admission limits.

Lists generate one record at a time from the parent registry. If definitions or
connections change while their list is being generated, it ends with `done/error`.
Treat records as provisional until `done/ok`; discard an incomplete view before
retrying. A single-record `get` or `open` reply retains its mapped metadata while
its JSON generator drains, without retaining serialized JSON. Shutdown interrupts
partial input and stalled output and closes accepted transports before workers.

Operations:

| Operation | Request fields | Result |
| --- | --- | --- |
| put | name, driver, kind, address, optional dsn/attributes | Durable full definition replacement |
| get | name | One definition, without credential attributes |
| list | none | Definitions individually, without credential attributes |
| delete | name | Durable removal, only with no pending/active connections |
| open | name, optional dsn/attributes | Pending connection and its one-time connection key |
| sessions | optional name | Connection metadata/status, without keys or attributes |
| close | connection_key | Close that UID's pending or active connection |

Only logical `driver: "mock"` is accepted in this slice. `dsn` is a named,
non-secret DSN; credential/user attributes belong in `attributes`. Limits are
63 bytes for names/driver, 255 for DSN, 1023 for attributes and platform-specific
Unix address length (at most 107 bytes here). Overflow fails; no truncation.
Names use ASCII letters/digits, dot, underscore and hyphen. Unknown JSON fields
are ignored by lonejson, duplicate known fields are rejected. Metadata is a
bounded resident record, not SQL or BLOB content. Lists flow one record at a time.

For `put`, omitted DSN/attributes become empty. For `open`, nonempty overrides
replace those of the selected definition only for this connection. They are
captured before key issuance and cannot be reassigned after attaching. Mutating
a definition with pending/active connections fails. Updates bind new endpoints
before committing and remove the old endpoint only after durable success.

## Connection key and data attachment

OS cryptographic entropy supplies 32 random bytes, encoded as 64 lowercase hex
characters. There is no deterministic fallback. Collision checks fail closed.
The key is bound to the creating peer UID, definition and credential snapshot.
Lists omit keys even for their creator; applications must retain the open result.
Keys are never persisted. Pending connections expire after 60 seconds.

Before sending the existing binary HELLO, the client sends exactly 64 ASCII key
bytes. The host checks its UID allowlist, native peer credentials, constant-time
key equality, selected definition, expiry and exclusive attachment. It sends
one byte: 0 means accepted, 1 means rejected. Unauthorized UIDs are disconnected
before parsing. No backend initialization occurs until those checks succeed.
An anonymous HELLO, a wrong/expired key, or a key for another endpoint cannot
reach the backend. The attached stream implicitly identifies its connection
for every subsequent binary operation; clients cannot supply another identity
inside a statement or parameter frame.

The connection supports successive statements on its one authenticated transport.
Disconnect, EOF, administrative close or daemon shutdown releases backend state
and invalidates the key. Simultaneous attachment and reconnect after disconnect
are rejected. Control `close` requires both key possession and creator UID.
Transport loss during a mutation can leave its outcome uncertain; no retry occurs.

## Persistence and startup

The file is a JSONL snapshot containing one object per reusable definition,
using required `type: "definition"`, name, driver, dsn, attributes, kind and
address. An empty file represents zero definitions. lonejson streams one record
at a time. Missing required fields, duplicate names/endpoints, bad JSON, invalid
drivers or over-limit records reject startup before service availability.

The state file and its `.lock` must be private regular files owned by the daemon
UID. Symlinks are refused. An exclusive lifetime lock prevents multiple writers.
State and lock paths are opened without blocking before their regular-file type
is checked, so a FIFO cannot hold startup waiting for another process.
Control and data pathname sockets cannot use the state file's reserved name,
even before the first save. This check resolves containing-directory identities
so relative paths and directory symlinks cannot bypass it.
A mutation streams the new records to a mode-0600 same-directory temporary file,
flushes and fsyncs it, renames it atomically, then fsyncs the directory. Before
rename, failure retains the previous registry and listeners. After rename,
directory-fsync failure produces `unknown` and stops the service rather than
claiming durability. The next start reads the actual on-disk state.

Flags/environment select startup settings; those settings are never loaded from
or written to the definitions file. File sockets remain default on Linux/macOS.
Both peer-UID gates default to the daemon UID; explicit lists replace them.
Linux uses SO_PEERCRED and Darwin uses getpeereid. Explicit allowlists permit
mode-0666 socket access but still require an allowed peer UID; parent-directory
access remains operator-managed. Abstract names provide no chmod protection.

The daemon forks isolated backend workers and keeps configuration in the parent.
Hosts may ignore SIGCHLD, use SA_NOCLDWAIT, or collect exited workers in a signal
handler. The parent releases a worker's descriptor, key and slot when waitpid
reports that it has already been reaped; it does not replace the host's handler.
`rdb_service_run` therefore requires a single-threaded host at entry. The 64
resident definitions and 128 connections are bounded operational limits. Native
transactions, reconnection/pooling and real iODBC remain later work.
