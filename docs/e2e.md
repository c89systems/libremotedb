# Streaming and container verification

The test-owned streaming backend validates generated input incrementally and
produces output incrementally. It retains one fixed pattern block and counters,
never a complete SQL/BLOB value or a spool file. It exercises the public service
and client APIs; the production daemon has no fault-injection flags. Test DSNs
select a transfer size in MiB. Per-connection test attributes select deliberate
worker crashes during execute, input, or output. These are fixture conventions,
not production database configuration.

`make test` includes actual small transfers, native memory bounds, deterministic
backpressure, managed service failures and cleanup, and lifecycle contracts.
The pressure test holds a backend input callback behind a pipe gate and proves
the public upload call cannot finish until consumption completes. It then pauses
output requests and verifies that the backend generates no additional chunks
until demand resumes. Transport tests also cut input frames before and after a
complete chunk, checking that only consumed data reached the backend.

`make test-streaming-stress` transfers **2048 MiB + 17 bytes in each direction**,
following a 1 MiB baseline. Set `REMOTEDB_STRESS_MIB=4096` to cross the wire's
32-bit low-limb boundary in an actual transfer. The generator and verifier use
bounded stack buffers. Both processes' peak resident memory may grow by at most
8 MiB, independently of the transfer size; every backend chunk is at most 4096
bytes. The test has a 300-second process alarm. This is a correctness/memory gate,
not a frozen throughput benchmark. Memcheck uses its own allocation accounting
and does not apply a native RSS bound to the instrumented process.

## Rootless Podman Kube

Prerequisites: native x86_64 Linux, rootless Podman with subordinate UID/GID
mappings and a working OCI runtime. The lifecycle resolves pinned Bootlin glibc
and musl collections. No registry image or database driver is required: both
images are built `FROM scratch`, tagged by their executable/runtime content,
and the pod's infrastructure process uses the same static client image.

The server uses Bootlin's dynamic glibc loader/libc; the client is static musl.
The manifest declares two service containers in one network namespace. It
publishes no ports and uses no host networking or privileged containers.
`--userns=keep-id` maps each service writer to the invoking host UID/GID.
A second permitted UID is used to prove that allowlist membership cannot take
over another UID's connection key. A third UID must fail both socket gates.

```sh
make deps PRESET=e2e-gnu
make deps PRESET=e2e-musl
make dev-up
make dev-ps
make dev-logs
make dev-down
make dev-reset
make test-e2e
```

`dev-up` defaults to pathname sockets. Set
`REMOTEDB_E2E_SOCKET_KIND=abstract` for Linux abstract sockets. Pathname mode
mounts the same socket directory into both containers. Abstract mode has no
shared socket mount; configuration persistence belongs only to the server.
Startup probes the real control API with a bounded readiness deadline.
`dev-render` rerenders a stopped environment from its recorded images and
settings. Stop/reset before changing the socket mode of a running environment.

`test-e2e` owns its services and refuses existing development state. It exercises
both socket modes, 64 MiB + 17 byte transfers, wrong-UID and key-owner rejection,
exclusive attachment, key invalidation, worker crashes, administrative close,
EOF during SQL/input/output, durable restart, and backend release. Set
`REMOTEDB_E2E_MIB` to select another transfer size (1 through 8192).
`REMOTEDB_E2E_KEEP_DEVSERVICES=1` retains services/state on failure, or the final
abstract environment on success, for inspection. Otherwise it tears down and
removes its own state. Run `dev-reset` before another e2e run.

All project-owned container storage, images, runtime files, state and temporary
work live under `build/devenv/`. Pod names are derived from the canonical
checkout path, so checkouts do not collide. Reset first uses the original
manifest with `podman kube down`, then resets only this dedicated container
store, then removes `build/devenv/` as the host user. It never invokes sudo or
changes ownership to make cleanup pass. Failure diagnostics include the command,
effective endpoint, pod state and container logs. Connection keys stay out of
logs; the restart test retains one client-owned mode-0600 key file temporarily.

`make test-all` and therefore `make prerelease` include the container gate.
`make prerelease-hardening` additionally includes the large streaming gate.
ARM and Darwin remain build/link/package coverage; these container tests do not
claim execution on those targets. Driver SQL semantics and native database
transaction behavior still require the eventual iODBC integration.
