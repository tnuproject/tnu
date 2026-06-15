# TODO_PKG.md — pkg + universe delivery

## Phase 0: Constraints
- Kernel persists VFS via TFS. No atomic rename syscall exists (but libc rename() exists by copy+unlink).
- Package manager must install by writing files into real FS and then syncing via existing persistence hooks.

## Phase 1: Inspect persistence/filesystem primitives
- [ ] Confirm TFS sync triggers and capacity constraints for large payloads.
- [ ] Decide staging approach: install into /var/lib/pkg/stage/<txn>/... then commit.

## Phase 2: Userspace `pkg` implementation
- [ ] Implement `pkg` CLI (commands per spec).
- [ ] Implement repo metadata loader + local repo mode (no HTTP yet unless HTTP syscalls exist).
- [ ] Implement installed DB persistence at `/var/lib/pkg/db.json`.
- [ ] Implement transactional-ish install:
  - [ ] extract into stage dir
  - [ ] verify manifests/dependencies
  - [ ] copy into final paths
  - [ ] sync + update DB
- [ ] Ensure root/sudo enforcement for sensitive actions.

## Phase 3: Host-side universe repo builder
- [ ] Define archive format.
- [ ] Define `repo.json` / `packages.json` schema.
- [ ] Add `tools/pkg/build.py` to generate archives + repo metadata + checksums.
- [ ] Add minimal universe example packages.

## Phase 4: Integration
- [ ] Update Makefile to build `pkg` and include it in rootfs.
- [ ] Add universe example repo files under `universe/` or `rootfs/`.
- [ ] Documentation: hosting on GitHub Pages / raw GitHub.

## Phase 5: Test
- [ ] `make all iso` and QEMU boot smoke test.
- [ ] In TNU: `pkg list`, `pkg repo list`, `pkg search`, `pkg info`, `pkg install <example>`.

