# libremotedb

## Remote database execution protocol and ODBC bridge

**Status:** Initial specification  
**Protocol:** RemoteDB v0  
**Primary language:** C89 / POSIX  
**Primary transport:** Unix domain sockets  
**Build system:** CMake  

---

## 1. Purpose

`libremotedb` provides a small, deterministic database-access interface across a process boundary.

The initial use case is allowing a fully static application, such as a musl-linked Vectis service, to use database drivers that cannot or should not be loaded into the application process. These may include proprietary ODBC drivers, drivers requiring glibc, drivers with complex runtime dependencies, or drivers whose licensing or packaging makes direct redistribution undesirable.

The database driver and its runtime environment remain on the server side. The client communicates with the server through a compact binary protocol.

The project consists of three primary build products:

```text
libremotedb
    Client library.

libremotedbsrv
    Server library implementing the RemoteDB protocol and database-driver
    execution surface.

remotedbd
    Reference server executable composed from libremotedbsrv.
```

The libraries and protocol are independent of any particular deployment topology.

A deployment may use a dedicated sidecar, a separate service process, a companion container, or another arrangement appropriate to the host application.

The first protocol implementation targets ODBC through an ODBC driver manager such as iODBC.

---

# 2. Design goals

RemoteDB SHALL provide:

1. A small C89 client library with no ODBC dependency.
2. A server library capable of exposing ODBC database connections through a binary protocol.
3. Full ordinary enterprise database access rather than a restricted query-only interface.
4. Prepared and direct SQL execution.
5. Local database transactions.
6. Typed input and output parameters.
7. Large-value streaming without requiring whole values to exist in memory.
8. Multiple result sets.
9. Stored procedure support.
10. Batch execution.
11. Database and schema metadata.
12. Complete SQL diagnostic propagation.
13. Capability discovery.
14. Driver isolation from the client process.
15. Explicit, bounded configuration of loadable database drivers.
16. A protocol whose wire representation does not depend on C structure layout, pointer size, libc, compiler ABI, or ODBC handle representation.

RemoteDB SHALL allow database-specific SQL to pass through unchanged.

Examples include:

```sql
INSERT INTO ...
UPDATE ...
DELETE FROM ...
MERGE ...
SELECT ...
SELECT ... FOR UPDATE
CREATE TABLE ...
ALTER TABLE ...
CALL ...
```

RemoteDB does not parse or normalize SQL.

---

# 3. Non-goals

RemoteDB v0 is not intended to:

- reproduce the complete ODBC C ABI across IPC;
- transmit pointers or native ODBC handles;
- implement a SQL parser;
- normalize SQL dialects;
- implement distributed transactions;
- implement XA;
- transparently combine multiple database connections;
- provide transparent failover between databases;
- provide a network-facing database proxy protocol;
- expose arbitrary shared-library loading;
- provide a general RPC framework;
- require gRPC, protobuf, HTTP, JSON-RPC, or another external serialization framework.

Distributed transaction coordination, where required, belongs outside this protocol.

For example, Vectis may use a separate XA-capable subsystem such as `liblockdc` for resources participating in distributed transactions.

RemoteDB v0 is concerned with a single database connection and its ordinary local transaction semantics.

---

# 4. Component architecture

```text
┌──────────────────────────────────┐
│ Application                      │
│                                  │
│ libremotedb                      │
└────────────────┬─────────────────┘
                 │
                 │ RemoteDB binary protocol
                 │ Unix domain socket
                 │
┌────────────────▼─────────────────┐
│ libremotedbsrv                   │
│                                  │
│ ODBC execution implementation    │
│                                  │
│ ODBC driver manager              │
│ e.g. iODBC                       │
└────────────────┬─────────────────┘
                 │
                 │ ODBC
                 │
┌────────────────▼─────────────────┐
│ User-provided database driver    │
│                                  │
│ Db2 / SQL Server / Oracle / ...  │
└──────────────────────────────────┘
```

The server process owns all native database handles.

The client never receives or manipulates:

```text
SQLHENV
SQLHDBC
SQLHSTMT
SQLHDESC
SQLPOINTER
```

RemoteDB defines its own identifiers and value representations.

---

# 5. Build products

The project SHALL expose three principal CMake targets.

## 5.1 Client

```text
remotedb
```

Output:

```text
libremotedb.a
libremotedb.so     optional
```

The client SHALL have no ODBC dependency.

The static form is the primary target.

---

## 5.2 Server library

```text
remotedbsrv
```

Output:

```text
libremotedbsrv.a
libremotedbsrv.so     optional
```

The server library contains:

- RemoteDB protocol decoding and encoding;
- connection state management;
- statement state management;
- parameter handling;
- result handling;
- streaming logic;
- ODBC adaptation;
- diagnostics;
- metadata operations;
- driver configuration and validation.

The server library SHALL NOT prescribe how the hosting process is packaged.

---

## 5.3 Reference server binary

```text
remotedbd
```

The binary SHALL provide a reference host for `libremotedbsrv`.

Its responsibilities SHOULD remain limited to:

- process initialization;
- configuration;
- socket lifecycle;
- accepted connection lifecycle;
- server-library initialization;
- logging;
- clean shutdown.

Database protocol behaviour SHALL remain in `libremotedbsrv`.

---

# 6. Example deployment

One possible deployment is a Kubernetes pod containing two containers.

```text
Pod
│
├── vectis
│   │
│   ├── FROM scratch
│   ├── fully static musl executable
│   └── libremotedb linked statically
│
└── remotedb
    │
    ├── small Linux userspace
    ├── glibc / dynamic loader
    ├── remotedbd
    ├── ODBC driver manager
    ├── approved database drivers
    └── driver configuration
```

Linux container e2e SHALL use two containers in one rootless Podman Kube pod,
sharing its network namespace. Abstract AF_UNIX sockets MAY be selected explicitly;
they require no shared socket volume. Merely joining the same bridge network does
not share the abstract socket namespace.

Pathname sockets SHALL remain the default on Linux and Darwin/macOS. Pathname
sockets across containers require a shared filesystem location. Abstract sockets
are a Linux-only opt-in; Darwin SHALL reject them as unsupported. A future change
to Linux's default requires a separate explicit decision.

The exact deployment topology is outside the core specification.

Other valid deployments include:

- both processes in the same filesystem namespace;
- separate containers;
- a dedicated local service;
- a supervised sidecar;
- a manually managed server process.

RemoteDB SHALL NOT depend on Kubernetes or containers.

---

# 7. Connection model

## 7.1 v0 model

RemoteDB v0 uses:

> one Unix socket connection per database connection.

The RemoteDB socket represents a single logical database session.

Therefore the protocol does not require a database-connection identifier in every message.

Conceptually:

```text
Unix socket A
    =
database connection A

Unix socket B
    =
database connection B
```

This makes:

- transaction ownership;
- session state;
- current database;
- isolation level;
- temporary tables;
- session variables;
- prepared statements;
- driver state

unambiguous.

---

## 7.2 Multiple connections

A client requiring several database connections in v0 SHALL establish several RemoteDB connections.

For example:

```text
rdb_connection *a;
rdb_connection *b;

rdb_open(..., &a);
rdb_open(..., &b);
```

Connection pooling and transparent multi-connection management MAY be added later.

The protocol SHALL not prevent this extension.

---

# 8. Server-created sockets

The reference daemon `remotedbd` SHALL provide a required control socket,
separate from database data sockets. `libremotedbsrv` SHALL own the configuration,
query, authorization and managed-connection behavior; the daemon is its process host.

The control subsystem SHALL support replacing, querying, listing and deleting
reusable database definitions; opening an authenticated connection; querying live
connections; and closing a connection by its connection key. SQL SHALL travel only
over the authenticated data transport, never the control socket.

A definition identifies a logical approved driver, named DSN, private connection
attributes and an endpoint consisting of an explicit kind (`pathname` or
`abstract`) and address. Every attachment creates an independent backend connection.
A reusable listener/definition is not a shared authenticated DB connection.

Opening a connection SHALL return a **connection key**, with 256 bits of OS-generated
cryptographic randomness encoded as 64 lowercase hexadecimal characters. It is the
unique credential identifying that connection. It SHALL bind the creator's peer UID,
selected definition, per-connection DSN/credential overrides and all backend state.
The implementation SHALL fail closed if OS entropy is unavailable; no timestamp,
PID, counter or deterministic PRNG fallback is permitted.

The data transport SHALL prove possession of this key and have the same peer UID
before any database protocol operation or backend initialization is accepted. A key
SHALL attach only to its selected definition, SHALL allow at most one active
transport, and SHALL never permit swapping another connection's user, credentials,
transaction, statements or result state. Changing or deleting a definition while
it has pending or active connections SHALL fail.

Keys SHALL be returned once by open, omitted from ordinary list/query responses,
and never logged or persisted. Closing/disconnecting destroys the connection,
releases its backend and invalidates its key. Unattached keys expire after 60 seconds.
Restart restores definitions, not keys or native DB handles. Reconnecting to retain
live transactions across transport loss is outside this slice; uncertain operations
SHALL NOT be retried automatically. Client applications may reuse an attached
connection for successive statements until disconnect.

Reusable definitions SHALL be persisted in one private, lonejson-backed JSONL file.
The daemon SHALL hold an exclusive lifetime lock. Loads and saves SHALL stream
records through lonejson; no complete JSON snapshot may be assembled in memory.
Mutations SHALL write a same-directory private temporary file, flush and fsync it,
atomically rename it, and fsync its directory before acknowledging durability.
Failed writes SHALL retain prior definitions/listeners. Failure to fsync the directory
after replacement SHALL report an uncertain outcome and stop the service.

Startup configuration SHALL come from flags or environment variables, with flags
taking precedence. It selects the state file, control endpoint and independent
control/data peer-UID allowlists. Startup settings SHALL NOT be written into the
persistent definitions file. Each allowlist defaults to the daemon UID; explicit
lists replace that default. Peer credentials SHALL be checked on both socket kinds
using the native Linux or Darwin credential API. Abstract sockets have no filesystem
permission protection. When explicit allowlists are used, the socket mode may permit
other UIDs to reach it; the peer-UID gate remains authoritative, and operators must
also provide searchable parent directories.

The mock driver remains the only approved backend until the iODBC backend using
pinned cpktdb and cpkt core SDKs is implemented. The providers keep separate install
prefixes and use ordinary CMake package discovery. cpktmisc is not required.
No arbitrary remote driver-library path loading or vendored iODBC is permitted.

---

# 9. Driver configuration

The server SHALL support database drivers supplied independently of the client.

The server SHALL NOT implicitly load arbitrary shared libraries supplied by protocol clients.

Drivers MUST be selected from server-side configuration.

A configuration SHOULD identify at least:

```text
logical driver name
driver manager
shared-library path
optional DSN
optional driver-specific attributes
allowed configuration properties
```

Example conceptual configuration:

```text
driver "db2" {
    library = "/var/lib/remotedb/drivers/db2/libdb2o.so"
}

driver "mssql" {
    library = "/var/lib/remotedb/drivers/msodbc/libmsodbcsql-18.so"
}
```

The exact syntax is not specified.

---

# 10. Driver whitelist

Dynamic driver loading MUST be constrained.

The server SHALL support one or more approved driver roots.

Example:

```text
/var/lib/remotedb/drivers
/opt/remotedb/drivers
```

A requested driver path MUST resolve within an approved root.

The server MUST reject:

- paths outside the whitelist;
- traversal outside the whitelist;
- unapproved absolute paths;
- driver identifiers not present in server configuration.

The client SHOULD normally select a logical driver name rather than provide a filesystem path.

Example:

```text
driver = "db2"
```

rather than:

```text
driver = "/whatever/the/client/wants.so"
```

---

# 11. Remote configuration

`libremotedbsrv` SHALL expose the remotely manageable configuration surface
defined in section 8.

Remote configuration MAY allow:

- listing configured driver definitions;
- listing available logical drivers;
- adding or updating approved driver definitions;
- validating a driver definition;
- opening a connection;
- closing a connection;
- inspecting server capabilities.

Remote configuration MUST NOT allow unrestricted loading of arbitrary filesystem paths.

Administrative operations SHALL use the separate control socket.

Administrative authorization is outside the database protocol proper.

---

# 12. Transport

RemoteDB v0 uses:

```text
AF_UNIX
SOCK_STREAM
```

The protocol is framed.

All reads and writes MUST correctly handle partial I/O.

The implementation SHALL provide equivalents of:

```text
read_full()
write_full()
```

No protocol operation may assume one `write()` corresponds to one `read()`.

---

# 13. Protocol framing

Every message begins with a fixed-width header.

Conceptual structure:

```c
struct rdb_header {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t flags;
    uint32_t statement_id;
    uint64_t payload_length;
};
```

This structure describes the wire fields only.

Implementations MUST encode and decode fields individually and MUST NOT send the native C structure directly.

The initial magic SHALL identify RemoteDB.

For example:

```text
"RDB0"
```

Protocol version:

```text
0
```

---

# 14. Wire encoding

RemoteDB v0 SHALL define:

```text
integer encoding: little endian
signed integer: two's complement
floating point: IEEE-754
text: UTF-8
length: explicit fixed-width integer
boolean: 0 or 1
```

Variable-length objects SHALL be length-prefixed.

NUL termination SHALL NOT be required on the wire.

A string may therefore contain:

```text
length = 4
bytes  = "test"
```

without an additional zero byte.

---

# 15. Request-response model

v0 SHALL be synchronous.

For every request:

```text
client → request
server → response
```

A new normal request SHALL NOT be sent until the previous operation has completed.

This avoids:

- request IDs;
- response reordering;
- protocol multiplexing;
- per-request concurrency bookkeeping.

Multiple concurrent database operations SHALL initially use multiple database connections.

Future protocol versions may add multiplexing.

---

# 16. Protocol operations

The initial protocol SHALL provide operations in the following areas:

```text
session
connection
transaction
statement
parameters
execution
results
streaming
procedures
batch execution
metadata
diagnostics
capabilities
control
```

---

# 17. Session operations

## HELLO

Negotiates protocol compatibility.

Request:

```text
protocol minimum
protocol maximum
client feature flags
```

Response:

```text
selected protocol version
server feature flags
implementation identifier
implementation version
maximum frame size
```

A protocol incompatibility MUST terminate the session cleanly.

---

# 18. Connection operations

## CONNECT

Creates the database connection represented by the socket.

Input SHOULD support:

```text
logical driver
DSN
connection string
username
password
database/catalog
connection timeout
requested attributes
```

The server converts these into the appropriate ODBC calls.

Typical underlying call:

```text
SQLDriverConnect
```

or:

```text
SQLConnect
```

The server SHOULD prefer non-interactive operation.

GUI prompting SHALL NOT be required.

---

## DISCONNECT

Terminates the database connection.

The server SHALL release:

```text
all statements
all pending streams
all connection resources
ODBC connection
ODBC environment resources where appropriate
```

---

# 19. Capability discovery

## GET_CAPABILITIES

Returns RemoteDB/server functionality.

Examples:

```text
transactions
transaction isolation selection
prepared statements
direct statements
parameter streaming
result streaming
batch parameters
multiple result sets
stored procedures
output parameters
metadata queries
query cancellation
scrollable cursors
generated keys
```

---

## GET_INFO

Exposes relevant database/driver capabilities.

The implementation SHOULD use:

```text
SQLGetInfo
```

where applicable.

Returned information MAY include:

```text
DBMS name
DBMS version
driver name
driver version
ODBC version
identifier quoting
maximum identifier length
transaction support
supported isolation levels
catalog support
schema support
procedure support
batch support
multiple result support
maximum statement length
maximum columns
```

Capabilities SHALL reflect the actual connected driver/database where possible.

---

# 20. Transaction model

RemoteDB SHALL expose explicit transaction operations.

The client API SHOULD include:

```c
rdb_tx_begin()
rdb_tx_commit()
rdb_tx_rollback()
```

---

## TX_BEGIN

Starts explicit transaction mode.

Request MAY specify:

```text
isolation level
read-only hint
driver-specific attributes
```

Supported isolation values SHOULD include:

```text
DEFAULT
READ_UNCOMMITTED
READ_COMMITTED
REPEATABLE_READ
SERIALIZABLE
```

Additional vendor-specific values MAY be represented through an extension mechanism.

An ODBC implementation will typically translate `TX_BEGIN` into connection attributes such as:

```text
SQL_ATTR_AUTOCOMMIT = SQL_AUTOCOMMIT_OFF
SQL_ATTR_TXN_ISOLATION = requested isolation
```

The physical database may begin the transaction lazily on first statement execution.

RemoteDB considers the transaction logically active after successful `TX_BEGIN`.

---

## TX_COMMIT

Commits the current transaction.

Typically maps to:

```text
SQLEndTran(..., SQL_COMMIT)
```

After successful commit, the connection returns to its normal non-explicit transaction state.

---

## TX_ROLLBACK

Rolls back the current transaction.

Typically maps to:

```text
SQLEndTran(..., SQL_ROLLBACK)
```

---

# 21. Savepoints

RemoteDB v0 SHALL NOT require a normalized savepoint protocol.

Savepoints MAY be performed through database SQL.

Examples:

```sql
SAVEPOINT x
```

```sql
ROLLBACK TO SAVEPOINT x
```

The exact SQL syntax remains database-specific.

A normalized savepoint API MAY be added later.

---

# 22. Row locking

RemoteDB SHALL not define separate row-lock operations.

Locking SQL is sent normally.

Example:

```sql
SELECT id, state
FROM jobs
WHERE id = ?
FOR UPDATE
```

Lock ownership therefore naturally belongs to the RemoteDB connection and its current transaction.

This preserves the database's own semantics.

---

# 23. Statement lifecycle

A statement is represented by a 32-bit RemoteDB statement identifier.

Example:

```text
statement_id = 17
```

The identifier is valid only within its database connection.

---

## PREPARE

Input:

```text
SQL text
optional statement attributes
```

Output:

```text
statement_id
parameter count where known
statement capabilities
```

Typical implementation:

```text
SQLAllocHandle(SQL_HANDLE_STMT)
SQLPrepare
```

---

## EXEC_DIRECT

Executes SQL without a prior client-visible prepare operation.

Input:

```text
SQL text
```

This may be implemented with:

```text
SQLExecDirect
```

or internally as:

```text
PREPARE
EXECUTE
```

The client-visible semantics SHALL remain equivalent.

---

## RESET_STATEMENT

Resets execution state while preserving the prepared statement where supported.

This SHOULD:

- close pending result cursors;
- discard unconsumed result data;
- clear parameter values;
- retain statement identity where safe.

---

## CLOSE_STATEMENT

Destroys the RemoteDB statement and corresponding native statement resources.

---

# 24. SQL operations

RemoteDB does not define separate opcodes for SQL CRUD operations.

The following all use the statement execution surface:

```sql
SELECT
INSERT
UPDATE
DELETE
MERGE
UPSERT
CALL
CREATE
ALTER
DROP
GRANT
REVOKE
```

Database-specific SQL remains valid.

---

# 25. Parameter model

Each statement parameter SHALL have:

```text
parameter index
direction
logical type
optional SQL type
optional native/vendor type
precision
scale
length
value representation
```

Parameter indexing SHALL follow ODBC conventions unless the client API explicitly presents another convention.

---

# 26. Parameter directions

RemoteDB SHALL support:

```text
IN
OUT
INOUT
RETURN
```

These map to the corresponding ODBC parameter directions.

This permits stored procedures and functions.

---

# 27. Logical value types

RemoteDB SHOULD initially support:

```text
NULL

BOOL

I8
I16
I32
I64

U8
U16
U32
U64

F32
F64

DECIMAL

UTF8
BYTES

DATE
TIME
TIMESTAMP
TIMESTAMP_TZ

UUID

JSON
BINARY_JSON

OPAQUE
```

---

# 28. Decimal representation

`DECIMAL` values SHALL preserve exact precision.

The wire representation SHOULD use:

```text
canonical decimal string
optional declared precision
optional declared scale
```

Example:

```text
precision = 20
scale     = 4
value     = "1234567890123456.7890"
```

RemoteDB SHALL NOT convert arbitrary decimals to binary floating point.

---

# 29. JSON

`JSON` represents UTF-8 textual JSON.

Example:

```text
type = JSON
length = ...
bytes = UTF-8 JSON document
```

The database may expose this as:

- textual JSON;
- a native JSON type;
- character data;
- a driver-specific type.

---

# 30. Binary JSON

`BINARY_JSON` represents an opaque binary JSON format understood by the target database or driver.

RemoteDB SHALL not interpret its contents.

The value may be mapped to:

```text
binary SQL type
vendor-specific SQL type
opaque driver binding
```

according to server-side driver support.

---

# 31. Opaque types

`OPAQUE` provides an escape hatch for vendor-specific types.

It SHALL allow the client and server to carry:

```text
native type identity
type name
binary payload
optional textual representation
```

This prevents RemoteDB's common type system from becoming a hard limit.

---

# 32. Ordinary parameters

Fixed-size scalar parameter values MAY be transmitted inline. All
variable-length parameter values MUST use streamed/chunked transfers, even
when small. Their declaration precedes execution and their bytes are supplied
only when requested during execution.

Conceptual message:

```text
PARAMETER

statement = 17
index     = 1
direction = IN
type      = I32
length    = 4

42
```

---

# 33. Large parameter streaming

Large values MUST be streamable.

Typical uses include:

```text
BLOB
CLOB
large JSON
binary JSON
documents
large XML
images
archives
```

The protocol SHALL support:

```text
PARAM_STREAM_BEGIN
PARAM_STREAM_DATA
PARAM_STREAM_END
```

Example:

```text
PARAM_STREAM_BEGIN
    statement = 17
    parameter = 2
    type      = BYTES
    length    = optional known length

PARAM_STREAM_DATA
    65536 bytes

PARAM_STREAM_DATA
    65536 bytes

...

PARAM_STREAM_END
```

The total size MAY be unknown when streaming begins.

Before EXECUTE, the client declares streamed parameters (type, direction, and
optional total length) without transferring their bytes. EXECUTE may return an
execution continuation requesting a specific parameter. PARAM_STREAM_BEGIN,
PARAM_STREAM_DATA, and PARAM_STREAM_END then supply that requested parameter.
PARAM_STREAM_END either requests another parameter or completes execution.
Each chunk receives a bounded acknowledgement, preserving synchronous flow
and backpressure. No unrelated operation may run during this continuation,
except explicit reset, disconnect, or supported cancellation.

---

# 34. ODBC streaming parameters

The ODBC backend SHOULD use data-at-execution semantics for streamed parameters.

Typical sequence:

```text
SQLBindParameter
    using data-at-execution indicator

SQLExecute
    → SQL_NEED_DATA

SQLParamData

SQLPutData
SQLPutData
SQLPutData
...

SQLParamData
```

The server MUST consume parameter chunks directly into the backend during execution.
Whole-value buffering, materialization, and temporary-file spooling are forbidden.
The backend may request streamed parameters in any order. The server must expose
the requested parameter ordinal and allow only that parameter until it is complete.

---

# 35. Execution

## EXECUTE

Executes a prepared statement.

The request references:

```text
statement_id
```

Parameters previously attached to the statement are used. Stream declarations
are attached before EXECUTE, but their data flows only while execution requests
it. EXECUTE is not complete while streamed input is pending.

The response SHALL identify the resulting execution state.

Possible outcomes include:

```text
no result set
result set available
multiple results possible
affected row count
output parameters pending
error
```

---

# 36. Affected row count

For DML operations such as:

```sql
INSERT
UPDATE
DELETE
MERGE
```

the server SHOULD report affected rows using:

```text
SQLRowCount
```

where supported.

The protocol SHALL use a signed 64-bit value.

A driver may report an unknown count.

---

# 37. Result metadata

Before returning result rows, the server SHALL expose result-set metadata.

For each column the protocol SHOULD include:

```text
ordinal
column label
column name where available
table name
schema name
catalog name

logical RemoteDB type
ODBC SQL type
database native type name

precision
scale
nullable
unsigned
display size
octet length
```

Implementation may use:

```text
SQLNumResultCols
SQLDescribeCol
SQLColAttribute
```

as needed.

---

# 38. Fetching rows

The initial client API SHOULD support forward-only result consumption.

Typical flow:

```text
EXECUTE
RESULT_META
FETCH
FETCH
FETCH
...
NO_DATA
```

Scrollable cursors MAY be added later or exposed through capability-dependent extensions.

---

# 39. Row representation

Each fetched row SHALL contain a sequence of typed cells.

A cell has one of:

```text
NULL
INLINE
STREAM
```

Example:

```text
ROW

column 1:
    INLINE I64
    123

column 2:
    STREAM UTF8
    # Value bytes are read through column chunks.

column 3:
    NULL

column 4:
    STREAM BYTES
```

---

# 40. Inline values

Fixed-size scalar values MAY be returned inline with the row. All
variable-length column values MUST use streamed/chunked transfers, even when
small. An inline threshold applies only to eligible fixed-size scalar values.

The implementation MAY define a configurable inline threshold.

Example:

```text
64 KiB
```

The exact threshold is not part of protocol semantics.

---

# 41. Result streaming

Large result values SHALL be streamable.

The protocol SHALL support:

```text
COLUMN_STREAM_BEGIN
COLUMN_STREAM_DATA
COLUMN_STREAM_END
```

Example:

```text
ROW_BEGIN

COLUMN
    I64 123

COLUMN_STREAM_BEGIN
    column = 2
    type = BYTES
    length = optional

COLUMN_STREAM_DATA
    ...

COLUMN_STREAM_DATA
    ...

COLUMN_STREAM_END

ROW_END
```

The server SHOULD retrieve streamed ODBC values incrementally using mechanisms such as:

```text
SQLGetData
```

The full value SHOULD NOT need to be held in server memory.

---

# 42. Streaming backpressure

The server SHALL not read indefinitely from the database if the client is not consuming data.

Unix socket flow control SHOULD provide natural backpressure.

Buffers SHOULD remain bounded.

RemoteDB implementations SHOULD avoid unbounded result queues.

---

# 43. Binary JSON streaming

`BINARY_JSON` SHALL support the same streaming mechanism as `BYTES`.

RemoteDB treats binary JSON as typed opaque bytes.

Example:

```text
COLUMN_STREAM_BEGIN
    type = BINARY_JSON
```

The server SHALL not require parsing or materializing the complete value.

---

# 44. Multiple result sets

RemoteDB SHALL support statements producing more than one result set.

The protocol SHALL provide:

```text
NEXT_RESULT
```

Typical sequence:

```text
EXECUTE

RESULT_META
FETCH...
NO_DATA

NEXT_RESULT

RESULT_META
FETCH...
NO_DATA

NEXT_RESULT

NO_MORE_RESULTS
```

The ODBC implementation will typically use:

```text
SQLMoreResults
```

---

# 45. Stored procedures

Stored procedures SHALL be fully supported through ordinary prepared or direct statements.

The protocol SHALL support:

- IN parameters;
- OUT parameters;
- INOUT parameters;
- return values;
- zero or more result sets;
- affected-row counts;
- multiple result sets;
- output parameters becoming available after results are drained.

A stored procedure execution may therefore produce:

```text
result set
result set
row count
output parameters
return value
```

The protocol SHALL preserve this order where the driver requires it.

---

# 46. Output parameters

After execution has reached the point where output parameters are available, the server SHALL expose:

```text
OUTPUT_PARAMETERS
```

Each value uses the normal RemoteDB type representation.

Large output parameters MAY use streaming.

---

# 47. Batch execution

RemoteDB SHOULD support batch execution of parameter sets.

Example SQL:

```sql
INSERT INTO orders(id, amount)
VALUES (?, ?)
```

The client may supply:

```text
1000 parameter sets
```

for one execution.

Protocol operation:

```text
EXECUTE_BATCH
```

The server MAY map this to ODBC parameter arrays.

---

# 48. Batch results

Where supported, batch execution SHALL return:

```text
processed count

per-row or per-set status:
    SUCCESS
    SUCCESS_WITH_INFO
    ERROR
    UNUSED
    UNKNOWN
```

Diagnostics SHOULD identify the relevant batch element when the underlying driver provides that information.

A driver incapable of reporting individual outcomes MAY provide only aggregate status.

---

# 49. Generated values

RemoteDB SHOULD allow retrieval of generated identifiers where supported.

The exact mechanism may be:

- returned result sets;
- driver-specific SQL;
- vendor SQL extensions;
- future normalized RemoteDB capability.

v0 SHALL not require a universal generated-key abstraction where the driver/database does not provide one consistently.

---

# 50. Metadata operations

RemoteDB SHALL expose common database metadata without requiring direct system-catalog SQL.

Initial metadata operations SHOULD include:

```text
TABLES
COLUMNS
PRIMARY_KEYS
FOREIGN_KEYS
STATISTICS
PROCEDURES
PROCEDURE_COLUMNS
TYPE_INFO
```

These correspond conceptually to ODBC catalog operations such as:

```text
SQLTables
SQLColumns
SQLPrimaryKeys
SQLForeignKeys
SQLStatistics
SQLProcedures
SQLProcedureColumns
SQLGetTypeInfo
```

Metadata operations SHOULD return ordinary RemoteDB result sets.

This allows the same row/result machinery to be reused.

---

# 51. Catalog, schema and table filters

Metadata operations SHOULD support filtering fields such as:

```text
catalog
schema
table
column
procedure
table type
```

`NULL` and empty-string semantics SHALL be distinguishable where required by ODBC.

---

# 52. Diagnostics

Diagnostics are a first-class part of RemoteDB.

Every server response SHALL contain a status.

Possible statuses SHOULD include:

```text
SUCCESS
SUCCESS_WITH_INFO
NO_DATA
ERROR
INVALID_HANDLE
UNSUPPORTED
PROTOCOL_ERROR
CONNECTION_LOST
CANCELLED
```

ODBC-specific intermediate states such as `SQL_NEED_DATA` MAY remain internal unless required by the public protocol.

---

# 53. Diagnostic records

A response MAY contain zero or more diagnostic records.

Each diagnostic record SHOULD contain:

```text
SQLSTATE
native error code
message text

optional row number
optional column number
optional server name
optional connection name
optional dynamic function
optional class origin
optional subclass origin
```

The minimum required fields are:

```text
SQLSTATE
native error code
message
```

---

# 54. ODBC diagnostic collection

The server SHALL collect ODBC diagnostics immediately after the operation which generated them.

Typical implementation:

```text
SQLGetDiagRec
SQLGetDiagField
```

The implementation MUST NOT perform unrelated calls on the same ODBC handle before diagnostics have been captured if doing so could overwrite diagnostic state.

---

# 55. SQL_SUCCESS_WITH_INFO

Warnings MUST NOT be silently discarded.

For example:

```text
string truncation
option value changed
driver warning
data conversion warning
```

shall be available to the RemoteDB caller.

The client API SHOULD expose both:

```text
operation result
diagnostic collection
```

---

# 56. SQLSTATE preservation

RemoteDB SHALL preserve the original five-character SQLSTATE.

Applications may therefore make portable decisions such as:

```text
23xxx    integrity constraint
40xxx    transaction rollback
08xxx    connection failure
```

without parsing vendor message strings.

The native error code SHALL also be preserved.

---

# 57. Driver-specific errors

Vendor-specific native codes SHALL remain intact.

Examples may include:

```text
Db2 SQLCODE
SQL Server native error number
Oracle native error
```

RemoteDB SHALL not attempt to translate these into a universal error-number namespace.

---

# 58. Client error model

The client SHOULD distinguish:

```text
transport/protocol error
RemoteDB server error
ODBC/database error
```

These are separate failure domains.

For example:

```text
Unix socket closed
```

is not equivalent to:

```text
SQLSTATE 08003
```

even if both imply loss of database access.

---

# 59. Timeouts

RemoteDB SHOULD support at least:

```text
connection timeout
login timeout
query timeout
```

These SHOULD map to ODBC facilities where available.

The server MUST report when a requested timeout is unsupported by the driver.

---

# 60. Cancellation

Graceful query cancellation MAY be supported.

Potential implementation mechanisms include:

```text
SQLCancel
SQLCancelHandle
```

However, v0 is synchronous and one request may block inside an ODBC driver.

Therefore graceful cancellation requires either:

- a separate control path;
- an additional thread;
- driver-specific support;
- process termination.

The core protocol SHALL reserve cancellation semantics without prescribing the implementation strategy.

A sidecar deployment MAY define process termination as hard cancellation.

---

# 61. Connection failure semantics

If the RemoteDB socket closes unexpectedly:

```text
client SHALL consider the database connection lost.
```

All of the following become invalid:

```text
transaction
statements
result sets
parameter streams
result streams
```

The client MUST NOT assume whether an in-flight database transaction committed.

The result may be:

```text
known committed
known rolled back
unknown
```

depending on where failure occurred.

RemoteDB SHALL not falsely convert uncertain outcomes into deterministic success or failure.

---

# 62. Transaction uncertainty

Particular care SHALL be taken when transport failure occurs during:

```text
COMMIT
ROLLBACK
EXECUTE
```

If the server dies after the database accepted a commit but before the client received the response, the client may have an uncertain outcome.

The API SHALL permit this distinction.

For example:

```text
RDB_ERR_TX_OUTCOME_UNKNOWN
```

This is preferable to reporting a normal rollback or transport error when the database outcome cannot be established.

---

# 63. Statement failure semantics

An SQL execution error SHALL normally leave the connection alive unless the driver reports otherwise.

Whether the current transaction remains usable depends on database semantics.

RemoteDB SHALL not automatically assume that every SQL error invalidates the transaction.

The caller may inspect:

```text
SQLSTATE
native error
connection state
transaction state where available
```

and decide whether to retry, rollback, or close.

---

# 64. Client API shape

The exact API is an implementation detail, but the following surface is representative.

```c
rdb_connect()
rdb_disconnect()

rdb_get_capabilities()
rdb_get_info()

rdb_tx_begin()
rdb_tx_commit()
rdb_tx_rollback()

rdb_prepare()
rdb_exec_direct()
rdb_stmt_close()
rdb_stmt_reset()

rdb_param_null()
rdb_param_i64()
rdb_param_u64()
rdb_param_f64()
rdb_param_decimal()
rdb_param_text()
rdb_param_bytes()
rdb_param_json()
rdb_param_stream_begin()
rdb_param_stream_write()
rdb_param_stream_end()

rdb_execute()
rdb_execute_batch()

rdb_result_columns()
rdb_fetch()
rdb_next_result()
rdb_row_count()

rdb_column_type()
rdb_column_i64()
rdb_column_text()
rdb_column_bytes()
rdb_column_stream()

rdb_output_parameter()

rdb_tables()
rdb_columns()
rdb_primary_keys()
rdb_foreign_keys()
rdb_procedures()

rdb_diag_count()
rdb_diag_get()
```

The public API SHOULD remain independent of ODBC types.

---

# 65. Streaming client API

Streaming APIs SHOULD permit caller-owned buffers.

Example:

```c
int
rdb_param_stream_write(
    rdb_stmt *stmt,
    unsigned param,
    const void *buf,
    size_t len);
```

Result streaming SHOULD similarly allow:

```c
int
rdb_column_read(
    rdb_stmt *stmt,
    unsigned column,
    void *buf,
    size_t cap,
    size_t *read,
    int *eof);
```

A callback interface MAY additionally be provided.

The core implementation SHOULD not require callbacks.

---

# 66. Memory constraints

The protocol and implementations SHALL be designed for bounded memory use.

In particular:

- a BLOB MUST NOT be fully buffered, materialized, or spooled;
- a result set SHALL not need to be fully buffered;
- a parameter batch MAY be chunked;
- metadata MAY stream as rows;
- diagnostics SHALL have explicit bounds and variable-length messages shall
  flow in chunks;
- frame sizes SHALL have configurable maxima.

---

# 67. Resource limits

The server SHOULD support configurable limits for:

```text
maximum frame size
maximum SQL text size
maximum inline value size
maximum statements per connection
maximum metadata rows where appropriate
maximum diagnostic message size
maximum concurrent server connections
maximum streamed chunk size
```

Protocol violations or exceeded limits MUST fail safely.

---

# 68. Authentication and authorization

The initial transport is intended for local Unix-domain communication.

The server MAY use:

```text
filesystem permissions
socket ownership
SO_PEERCRED
container/pod isolation
```

for access control.

Remote database credentials SHALL not be exposed to unrelated local clients.

Administrative configuration and ordinary database sessions SHOULD use separate access controls.

---

# 69. Credential handling

Credentials MAY be supplied:

- through connection requests;
- through server-side named configuration;
- through environment/secrets integration;
- through external credential providers.

The protocol SHALL support credentials without requiring them to be stored in persistent configuration.

Passwords SHALL never be logged by default.

---

# 70. Driver isolation

One motivation for RemoteDB is fault isolation.

A database driver may:

- crash;
- corrupt memory;
- leak;
- deadlock;
- start threads;
- install signal handlers;
- load its own dependencies.

A deployment MAY isolate such drivers in separate server processes.

RemoteDB SHALL not require all connections to exist in one process.

---

# 71. Process-per-connection compatibility

The protocol SHALL work naturally where each database connection is backed by a dedicated server process.

```text
client connection
       │
       ▼
dedicated remotedb process
       │
       ▼
one database connection
```

This model provides particularly clear:

```text
session ownership
transaction ownership
failure containment
cleanup
```

but is not mandated.

---

# 72. Server multiplexing

`libremotedbsrv` MAY also be embedded in a process serving multiple sockets and database connections.

If used this way, state belonging to different database sessions MUST remain isolated.

The protocol itself remains one-connection-per-socket in v0.

---

# 73. Threading

The server library SHALL not require a particular threading model.

A host may choose:

```text
one process per connection
one thread per connection
event loop
worker pool
```

Provided that ODBC driver's threading constraints are respected.

v0 protocol processing for an individual connection remains sequential.

---

# 74. ODBC environment handling

The server implementation MAY reuse one ODBC environment across connections where supported.

Alternatively, each connection MAY own its own environment.

This is an implementation choice.

Connection and statement handles MUST never be shared incorrectly across RemoteDB sessions.

---

# 75. ODBC version

The server SHOULD request an ODBC version suitable for modern drivers, normally ODBC 3.x.

The exact driver-manager configuration remains server-side.

Protocol users SHALL not depend on the specific ODBC driver-manager implementation.

---

# 76. Driver-manager independence

The protocol SHALL not expose iODBC-specific structures.

Although the initial implementation may use iODBC, `libremotedbsrv` SHOULD keep its internal database-driver layer replaceable.

Possible future implementations may use:

```text
unixODBC
native database APIs
test drivers
mock backends
```

without changing `libremotedb`.

---

# 77. Native extensions

Vendor-specific functionality MAY be exposed later through extension namespaces.

An extension SHALL be identified by:

```text
extension identifier
extension version
operation identifier
```

Core protocol compatibility SHALL not depend on optional vendor extensions.

---

# 78. Protocol extensibility

Unknown optional capabilities SHALL be ignored where safe.

Unknown required protocol operations SHALL return:

```text
UNSUPPORTED
```

New opcodes MUST NOT silently change the meaning of existing opcodes.

---

# 79. Version negotiation

The client SHALL advertise supported protocol range.

Example:

```text
minimum = 0
maximum = 0
```

The server returns the highest mutually supported version.

No common protocol version results in clean connection rejection.

---

# 80. Testing

The project SHALL provide tests for at least:

### Protocol

- frame encode/decode;
- fragmented reads;
- fragmented writes;
- invalid magic;
- unsupported protocol;
- malformed payload lengths;
- oversized frames;
- unknown opcodes.

### Statements

- direct SELECT;
- prepared SELECT;
- INSERT;
- UPDATE;
- DELETE;
- DDL;
- parameters;
- NULL values.

### Transactions

- begin;
- commit;
- rollback;
- isolation selection;
- row lock within transaction;
- failure during transaction;
- transport loss during transaction.

### Types

- integer boundaries;
- floating point;
- decimal precision;
- Unicode;
- embedded NUL in bytes;
- JSON;
- binary JSON;
- dates and timestamps;
- NULL.

### Streaming

- multi-gigabyte conceptual BLOB using generated data;
- unknown input length;
- result streaming;
- partial socket writes;
- client backpressure;
- interrupted stream.

### Stored procedures

- IN;
- OUT;
- INOUT;
- return value;
- multiple result sets.

### Diagnostics

- SQL syntax error;
- constraint violation;
- deadlock/serialization error where available;
- truncation warning;
- multiple diagnostic records.

### Metadata

- tables;
- columns;
- PK;
- FK;
- procedures;
- type info.

### Failure isolation

- driver connection failure;
- server termination;
- malformed driver configuration;
- missing driver;
- dependency load failure.

---

# 81. Reference acceptance scenarios

## Basic CRUD

```text
CONNECT
TX_BEGIN

PREPARE INSERT
PARAMETERS
EXECUTE

PREPARE UPDATE
PARAMETERS
EXECUTE

PREPARE SELECT ... FOR UPDATE
PARAMETERS
EXECUTE
FETCH

TX_COMMIT
DISCONNECT
```

---

## Large BLOB upload

```text
PREPARE INSERT

PARAM scalar
DECLARE streamed parameter

EXECUTE
    → NEED_INPUT(parameter ordinal)

PARAM_STREAM_BEGIN
DATA
DATA
DATA
...
PARAM_STREAM_END
    → next NEED_INPUT or execution completed

COMMIT
```

No component requires the complete BLOB in memory.

---

## Large BLOB retrieval

```text
PREPARE SELECT
EXECUTE

RESULT_META
FETCH

COLUMN_STREAM_BEGIN
DATA
DATA
DATA
...
COLUMN_STREAM_END
```

---

## Stored procedure

```text
PREPARE CALL

PARAM IN
PARAM INOUT
PARAM OUT

EXECUTE

RESULT_SET
FETCH...
NEXT_RESULT

RESULT_SET
FETCH...
NEXT_RESULT

NO_MORE_RESULTS

OUTPUT_PARAMETERS
```

---

## Two database connections

v0 uses two independent client connections:

```text
libremotedb connection A
    ↓
socket A
    ↓
database session A

libremotedb connection B
    ↓
socket B
    ↓
database session B
```

No special multi-connection protocol support is required.

---

# 82. Initial implementation priorities

A sensible implementation sequence is:

1. framing;
2. HELLO;
3. CONNECT/DISCONNECT;
4. direct SQL execution;
5. diagnostics;
6. prepared statements;
7. typed parameters;
8. forward-only result sets;
9. transactions;
10. streamed input parameters;
11. streamed result columns;
12. multiple result sets;
13. stored procedure output parameters;
14. metadata;
15. batch execution;
16. timeout/cancellation support;
17. administrative driver configuration.

Each stage SHOULD preserve protocol extensibility.

---

# 83. v0 required feature set

A v0 implementation SHOULD not be considered complete until it supports:

```text
Unix-domain socket transport

CONNECT / DISCONNECT

HELLO / capability negotiation

EXEC_DIRECT

PREPARE / EXECUTE

INSERT / SELECT / UPDATE / DELETE through SQL

typed parameters

NULL

transactions:
    BEGIN
    COMMIT
    ROLLBACK
    isolation selection

SELECT FOR UPDATE through SQL

affected row counts

forward-only result sets

result metadata

multiple result sets

stored procedures:
    IN
    OUT
    INOUT
    return values

large BLOB input streaming

large BLOB output streaming

JSON

binary JSON / opaque binary types

batch execution

SQLSTATE

native database errors

multiple diagnostic records

SQL_SUCCESS_WITH_INFO propagation

common metadata queries

bounded resource usage
```

---

# 84. Future directions

Potential later extensions include:

```text
transparent connection pools

multi-connection client manager

server-side prepared-statement cache

shared-memory transfer

memfd + SCM_RIGHTS for very large objects

asynchronous requests

query cancellation control channel

scrollable cursors

cursor updates

bulk-copy extensions

driver-specific extension namespaces

TLS/TCP transport

remote hosts

authentication protocol

metrics and tracing

XA-specific provider extensions
```

These SHALL not complicate the v0 protocol unnecessarily.

---

# 85. Architectural principle

RemoteDB is not intended to reproduce ODBC's in-process memory model.

It preserves the useful semantics:

```text
connections
transactions
statements
parameters
typed values
streaming
results
metadata
diagnostics
capabilities
```

while replacing pointer-oriented ODBC interaction with a stable process boundary.

The resulting architecture allows a small static client application to use enterprise database drivers without loading those drivers, their libc assumptions, or their dependency graphs into the application process.

For Vectis, this permits the core runtime to remain a fully static musl executable while database-specific compatibility can be placed behind an isolated RemoteDB server when required.

The process boundary is therefore not merely a compatibility workaround. It is an explicit isolation boundary between the application runtime and third-party database-driver code.


# 86. Bootstrap slice

The initial implementation establishes the lifecycle and a mock-backed streaming
transport. Its exact wire assignments and supported subset are described in
`docs/wire.md`. It is not a complete v0 implementation. SQL text is transferred
in chunks without parsing or normalization. Backend mock tests prove byte
preservation and execution/stream state, not vendor SQL semantics. iODBC is
reserved for a future backend using pinned c89systems/cpktdb and c89systems/cpkt
SDKs; no driver manager is vendored. c89systems/cpktmisc is an independent optional
provider and is not a dependency of this project.
