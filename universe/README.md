# TNU Universe

`universe` is the default package repository for TNU.

The repository layout is intentionally simple while the OS grows its POSIX,
networking, and package verification layers:

```text
packages/
  name/
    manifest
    files/
      absolute/install/path
```

`INDEX` contains the package list:

```text
name|version|architecture|category|summary
```

`pkg install doom` and `pkg install nano` copy the package payload from
`files/` into the running system and register the manifest in
`/var/db/pkg/installed`.

The canonical hosted repository is:

```text
https://github.com/tnuproject/universe/
```

TNU reads raw package metadata from:

```text
https://raw.githubusercontent.com/tnuproject/universe/main
```

The install ISO also includes a local mirror at:

```text
/usr/share/pkg/repos/universe
```

Run `pkg sync` first. Until TNU's TCP/HTTP userspace transport is finished,
`pkg sync` seeds `/var/cache/pkg/repos/universe` from the ISO mirror; afterward
the same command can download from the GitHub raw URL.
