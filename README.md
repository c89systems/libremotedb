# libremotedb

Strict C89 client/server libraries for a synchronous, bounded, Unix-socket
database protocol. This bootstrap implements the transport and an explicit
mock backend. It is not a complete RemoteDB v0 or an ODBC implementation.

The client has no ODBC dependency. The planned iODBC backend will use a
checksum-pinned [cpktdb](https://github.com/c89systems/cpktdb) SDK and its
[cpkt](https://github.com/c89systems/cpkt) core prerequisite, each installed in
its own prefix and discovered through `CMAKE_PREFIX_PATH`. Public facade headers
retain the `<cpkt/...>` namespace. The third provider,
[cpktmisc](https://github.com/c89systems/cpktmisc), supplies unrelated optional
components and is not required by libremotedb. No driver manager is vendored here.
Unsupported dependency modes fail rather than selecting a host ODBC install.

## Build and verify

Use `make help` for the authoritative command index.

```sh
make deps-debug
make build
make test
make valgrind
make finalize-slice
make deps-cross
make deps-fuzz
make prerelease
```

Linux targets are x86_64, aarch64, and armhf, each with Bootlin glibc and musl
collections pinned to stable-2026.08-1 by the checked-in resolver. Host compilers
and binutils are never Linux fallbacks. macOS targets arm64, minimum macOS 11,
through a developer-supplied osxcross SDK. The resolver selects the newest
complete Darwin 25.x prefix; `CPKT_OSXCROSS_HOST` pins an exact installed prefix.
`OSXCROSS_ROOT` overrides the collection location. Darwin configure/link/package
commands select the target linker and place its bin directory first on PATH.
The resolver provisions pinned host MIG after a working local SDK is supplied.
Preparation is explicit: `make deps-cross` prepares all target collections and
dependency SDKs; `make deps-fuzz` prepares native AFL++. Ordinary builds,
configuration, `try_compile`, discovery and environment exports never provision
toolchains. Missing tools fail with the required preparation command.

Toolchains use the shared `CPKT_TOOLCHAIN_CACHE`. Pinned lonejson 0.44.0 SDK archives
use `CPKT_DEPENDENCY_CACHE`. Their defaults are
`${XDG_CACHE_HOME:-$HOME/.cache}/cpkt/toolchains` and
`${XDG_CACHE_HOME:-$HOME/.cache}/cpkt/deps`, respectively. Explicit cache overrides
take precedence. The Darwin core is rebuilt from the pinned upstream
release source to preserve macOS 11; the published lonejson SDK requires macOS 15.
Binary SDKs include only lonejson’s core static closure, without optional adapters. Repository cleans preserve both shared caches.
The host provides Bash 4.4+, CMake 3.25+, Ninja, pkg-config, Valgrind,
LLVM/Clang (including clang-format and clangd), and the resolver prerequisites
curl or wget, GNU tar, gzip, xz, sha256sum, flock, bison, flex, and ordinary build
tools. Local test fixtures also require Python 3 and OpenSSH tools. Python only
serves fixture data; Make, Bash, CMake and CTest own the lifecycle and tests.
The helper collection in `scripts/` is vendored from the current
`pkt-systems-cmake-lifecycle` skill, including its resolver regression fixtures.

Each checkout owns `build/`, `dist/` and local `.cache/`; cache overrides must
select its own `build/` or `.cache/`, or the global cpkt cache. `make clean`
removes the entire owned generated trees, including hidden entries, while
preserving shared archives, toolchains and workstation prerequisites.
Repository build and test commands are serialized with one build job. The
invoking terminal owns cancellation of the whole job/process group.

`make test` uses a CMake-declared suite success stamp. Binary, test, script or
configuration changes invalidate it. Run `ctest --preset debug` to force a
fresh suite, and remove `build/debug/local-tests-passed` after restoring files
with preserved timestamps or changing untracked execution inputs. Host-only
fixtures run in Debug; target and memory checks retain their own configurations.

Native tests and examples select the Bootlin loader and private runtime paths
at link time. Shipped SDK libraries and executables contain no cache paths.
Native Memcheck and AFL++ run without emulation. Cross targets receive
compile/link and artifact checks; this project has not selected QEMU execution.

## Streaming contract

SQL and bulk parameter bytes pass directly through bounded chunks. The server
uses a fixed 4 KiB scratch buffer. It never builds the full SQL, BLOB, or result
in memory or a temporary file. The backend consumes a chunk before its
acknowledgement, so a slow consumer propagates backpressure to the producer.
Caller buffers are borrowed only during a synchronous call.

The first slice has one statement per session. SQL is written between
`rdb_connection_sql_begin` and `rdb_connection_sql_end`. Execution can return
`RDB_NEED_INPUT`; the response identifies the backend-requested parameter.
Write its chunks, then end it. The next response requests another parameter or
completes execution. Output is pulled with `rdb_connection_column_read`.
Reset discards the statement; disconnect releases the backend session.

The mock fingerprints SQL and input incrementally and generates output
incrementally. Tests exercise vendor SQL examples without interpreting them.
These tests establish unchanged transfer and state-machine correctness.
Database dialect, locking, transaction, conversion, and driver behavior require
later real-driver integration tests.

See [the product spec](docs/spec.md) and [the exact bootstrap wire contract](docs/wire.md).
The remaining v0 work includes typed parameters/results, multiple statements,
diagnostic records, transactions, metadata, batches, procedures, and iODBC.
The backend interface deliberately leaves ODBC types out of public headers.

## Managed mock daemon

```sh
mkdir -p build/demo
build/x86_64-linux-gnu-release/remotedbd --mock \
  --control build/demo/control.sock --state build/demo/definitions.jsonl
# In another terminal:
build/x86_64-linux-gnu-release/remotedbctl build/demo/control.sock \
  put documents mock pathname build/demo/documents.sock
build/x86_64-linux-gnu-release/remotedbctl build/demo/control.sock list
# Open returns the connection key once; pass it directly to the consumer:
build/x86_64-linux-gnu-release/remotedbctl build/demo/control.sock open documents | \
  build/debug/stream-example build/demo/documents.sock
```

`remotedbd` hosts concurrent connections until SIGINT/SIGTERM. Each gets an
isolated backend; disconnect releases it. Definitions persist across restart,
while connection keys and per-connection credentials do not. File updates use
an exclusive store lock, atomic replacement and file/directory fsync. The state
file must be private, owned by the daemon UID, with an existing parent directory.
Existing socket paths are never removed to make a bind succeed; normal shutdown
removes only sockets still matching the daemon's original inode.

Flags override `REMOTEDB_CONTROL`, `REMOTEDB_STATE`, `REMOTEDB_SOCKET_KIND`,
`REMOTEDB_CONTROL_UIDS` and `REMOTEDB_DATA_UIDS`. Defaults are
`./remotedb-control.sock`, `./remotedb.jsonl`, pathname sockets, and the daemon UID
for both gates. Explicit comma-separated UID lists replace those defaults.
Explicit allowlists use socket mode 0666 with mandatory peer-credential checks;
parent directories must also permit the selected users to reach the socket.
Without explicit lists the socket mode is 0600. The current driver is always
mock; no real database connection is implied.

For Linux abstract sockets, use `remotedbd --socket-kind abstract --control NAME`
and `remotedbctl --abstract NAME ...`; a definition can separately select an
abstract data address. The client C API accepts an explicit `rdb_endpoint`.
Darwin rejects abstract endpoints. Linux container e2e will place both containers
in one rootless Podman Kube pod sharing its network namespace. Its harness is
still future work; pathname sockets remain the default.

`rdb_control_query` streams metadata records to a consumer callback. `open`
returns a random 256-bit **connection key**, bound to the creator UID and selected
definition. Use `rdb_open_key` to authenticate before the binary protocol begins.
Ordinary queries never disclose keys or credential attributes. A connection
permits one attached transport and successive SQL statements; disconnect
invalidates the key. Pending keys expire after 60 seconds. Credentials may be
supplied in the C control request's attributes for `open`; they are snapshotted
for that connection and not persisted. Never log keys or put them in argv.
`remotedbctl close` reads its key from stdin.

The control and persistence formats are described in [control.md](docs/control.md).
The bootstrap bounds definitions at 64 and pending/active connections at 128.
These limits concern resident metadata, never total SQL/BLOB stream sizes.

## SDKs and local release

`make release-matrix` builds and verifies all seven binary SDKs under `dist/`.
No target is silently skipped. Every SDK contains static and shared client and
server libraries, the explicit mock reference host, C89 headers, CMake and
pkg-config metadata, examples, the released lonejson SDK and license, and dependency provenance.
Linux mock reference hosts are statically linked; future iODBC hosts will need
the appropriate dynamic driver runtime.

Downstream CMake users select `remotedb::remotedb_static`,
`remotedb::remotedb_shared`, `remotedb::remotedbsrv_static`, or
`remotedb::remotedbsrv_shared` after `find_package(remotedb CONFIG REQUIRED)`.
pkg-config names are `remotedb` and `remotedbsrv`; use `--static` for static
link closure. Shared libraries expose only source-controlled export allowlists.
The initial SONAME is 0. Future public ABI changes advance from the latest
released ABI, independently of the pre-1.0 product version.

Git builds resolve an exact lightweight `vX.Y.Z` tag or explicit
`REMOTEDB_VERSION_OVERRIDE`, otherwise 0.0.0. Git builds ignore `VERSION`.
Source archives carry an injected `VERSION` and explicit `RELEASE_MANIFEST`.
The checksum manifest is `libremotedb-VERSION-CHECKSUMS`; artifact verification
extracts every listed archive, scans privacy and loader metadata, checks exports,
and builds downstream consumers. Native consumers are also executed.

Archives use stable order, timestamps, ownership, modes and gzip metadata.
Binary rehearsals keep their seven-SDK checksum inventory under `build/`.
`make package-checksums` requires the complete seven-SDK plus source shipment;
it rejects stale and unlisted artifacts. Verification checks final extracted
bytes, dependency provenance, architecture, exports, loader paths, relocation,
CMake/pkg-config consumers and native consumer execution.

`make release` is the final clean local gate. Before removing any generated
state, it requires the resolved default branch, a clean committed worktree,
a verified signed HEAD, its exact lightweight release tag, and no candidate
version override. It then cleans once, explicitly prepares dependencies, runs
local proof and the binary matrix, and reconstructs the source archive. Develop
and verify on one feature branch, squash onto the local default branch, and
amend/sign that commit before tagging and running the final gate. Version/tag
variation tests use disposable Git fixtures; they never alter this checkout's tags.
The gate does not push, tag or publish. No remote CI is required. A failed final
gate stops; repairing and retrying requires a separate authorized iteration.

Streaming stress and rootless two-container Podman verification are documented
in [docs/e2e.md](docs/e2e.md). `make test-e2e` proves pathname and Linux abstract
transport between a static musl client and dynamic glibc test host, including
connection-key isolation, faults, restart and unprivileged cleanup.
`make test-streaming-stress` proves actual multi-gigabyte transfer with bounded
memory. These fixtures exercise the production library boundary while iODBC is
pending.
