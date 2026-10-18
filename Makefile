SHELL := /usr/bin/env bash
.NOTPARALLEL:
PRESET ?= x86_64-linux-gnu-release
DEPENDENCY ?= lonejson
export REMOTEDB_VERSION_OVERRIDE
.PHONY: help build build-debug build-host build-release test test-debug test-host test-release test-all valgrind deps deps-debug deps-release deps-cross deps-fuzz cross-build cross-test test-cross format format-check finalize-slice fuzz-smoke fuzz fuzz-long bench dev-up dev-down dev-reset dev-render dev-ps dev-logs test-e2e test-streaming-stress package package-source package-source-smoke package-checksums package-verify verify-release-archives verify-release-privacy test-install-tree release-matrix prerelease prerelease-hardening print-release-version lifecycle-version-contract release clean clean-dist
help:
	@printf '%s\n' \
	  'deps PRESET=<preset>                       Prepare the pinned toolchain and lonejson SDK' \
	  'deps-debug, deps-release, deps-cross, deps-fuzz  Explicit tool and SDK preparation' \
	  'build, build-debug, build-host              Native Debug build (requires prepared tools)' \
	  'build-release                             Native GNU Release build' \
	  'test, test-debug, test-host                 Debug CTest gate with native dependency tracking' \
	  'test-release PRESET=<preset>               Selected Release CTest gate' \
	  'test-all                                  Debug, GNU/musl Release, Memcheck, fuzz and Podman' \
	  'cross-build, cross-test, test-cross         All target build/link checks; no cross runner' \
	  'format, format-check, finalize-slice        Format, check and native development gate' \
	  'valgrind, fuzz-smoke, fuzz, fuzz-long        Native Memcheck and AFL++ (5/30/300 seconds)' \
	  'dev-up, dev-down, dev-reset, dev-render, dev-ps, dev-logs  Rootless Podman Kube fixtures' \
	  'test-e2e, test-streaming-stress             Container isolation / actual multi-GiB transfer' \
	  'package PRESET=<preset>                    Deterministic CMake-installed SDK archive' \
	  'test-install-tree                         Archive, relocation and installed consumers' \
	  'release-matrix                            All seven SDKs and binary-scope verification' \
	  'package-source, package-source-smoke        Explicit source archive and clean reconstruction' \
	  'package-checksums, package-verify            Complete release inventory and final-byte checks' \
	  'prerelease, prerelease-hardening            Incremental local proof; no publication' \
	  'lifecycle-version-contract                 Read-only final release identity validation' \
	  'release                                   Signed, tagged, clean local default-branch release gate' \
	  'clean, clean-dist                          Remove owned generated state; preserve global caches' \
	  'Variables: PRESET, REMOTEDB_VERSION_OVERRIDE, CPKT_TOOLCHAIN_CACHE, CPKT_DEPENDENCY_CACHE,' \
	  'OSXCROSS_ROOT, CPKT_OSXCROSS_HOST, REMOTEDB_E2E_SOCKET_KIND, REMOTEDB_E2E_MIB,' \
	  'REMOTEDB_E2E_KEEP_DEVSERVICES, REMOTEDB_STRESS_MIB'
build build-debug build-host:
	@scripts/build.sh debug
build-release:
	@scripts/build.sh x86_64-linux-gnu-release
test test-debug test-host:
	@scripts/build.sh debug check-local
test-release:
	@scripts/build.sh "$(PRESET)" check-local
valgrind:
	@scripts/build.sh valgrind check-local
deps:
	@test "$(DEPENDENCY)" = lonejson || { echo 'Only DEPENDENCY=lonejson is available' >&2; exit 2; }
	@scripts/prepare.sh "$(PRESET)"
deps-debug:
	@$(MAKE) deps PRESET=debug
deps-release:
	@$(MAKE) deps PRESET=x86_64-linux-gnu-release
deps-cross:
	@while IFS= read -r target; do scripts/prepare.sh "$$target-release" || exit; done < <(scripts/cpkt-toolchains.sh targets)
deps-fuzz:
	@scripts/prepare.sh fuzz
cross-build cross-test test-cross:
	@while IFS= read -r target; do scripts/build.sh "$$target-release" || exit; done < <(scripts/cpkt-toolchains.sh targets)
test-all:
	@$(MAKE) test
	@$(MAKE) test-release PRESET=x86_64-linux-gnu-release
	@$(MAKE) test-release PRESET=x86_64-linux-musl-release
	@$(MAKE) valgrind
	@$(MAKE) fuzz-smoke
	@$(MAKE) test-e2e
format:
	@scripts/format.sh write
format-check:
	@scripts/format.sh check
finalize-slice: format test format-check
fuzz-smoke:
	@scripts/fuzz.sh 5
fuzz:
	@scripts/fuzz.sh 30
fuzz-long:
	@scripts/fuzz.sh 300
bench:
	@scripts/build.sh debug
	@time build/debug/test-remotedb streaming
dev-up dev-down dev-reset dev-render dev-ps dev-logs:
	@scripts/devenv.sh $(patsubst dev-%,%,$@)
test-e2e:
	@scripts/devenv.sh test
test-streaming-stress:
	@scripts/build.sh x86_64-linux-gnu-release
	@build/x86_64-linux-gnu-release/test-streaming transfer "$${REMOTEDB_STRESS_MIB:-2048}"
package:
	@scripts/package.sh sdk "$(PRESET)"
package-source:
	@scripts/package.sh source
package-source-smoke:
	@scripts/package.sh source-smoke
package-checksums:
	@scripts/package.sh checksums
package-verify verify-release-archives verify-release-privacy:
	@scripts/package.sh verify
test-install-tree:
	@scripts/package.sh sdk "$(PRESET)"
	@scripts/package.sh verify-one "$(PRESET)"
release-matrix:
	@scripts/package.sh matrix
prerelease: format-check test-all release-matrix
prerelease-hardening: prerelease test-streaming-stress fuzz-long
print-release-version:
	@scripts/release_version.sh
lifecycle-version-contract:
	@scripts/release-check.sh
release:
	@scripts/release-check.sh
	@$(MAKE) clean
	@$(MAKE) deps-cross
	@$(MAKE) deps-fuzz
	@$(MAKE) prerelease
	@$(MAKE) package-source
	@$(MAKE) package-source-smoke
	@$(MAKE) package-checksums
	@$(MAKE) package-verify
clean:
	@scripts/clean.sh all
clean-dist:
	@scripts/clean.sh dist
