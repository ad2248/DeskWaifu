# DeskWaifu CI runner

The `deskwaifu-ci-runner.service` user service on `st` runs container jobs through
rootless Podman. Its configuration is
`~/.config/deskwaifu-ci-runner/config.yaml`:

```yaml
runner:
  capacity: 4
container:
  options: --cpus=8 --memory=8g --memory-swap=8g
```

These are excerpts, not a replacement configuration. Preserve the registered
runner identity, image-ID-pinned `deskwaifu-arch` label, `force_pull: false`,
`docker_host: "-"`, `privileged: false`, and `valid_volumes: []`.

Both the main build and the render-target variant builds explicitly use
`cmake --build ... --parallel 16`. Do not derive compiler parallelism from
`nproc`: the compiler job count is intentionally higher than the container's
8-CPU quota. `--cpus=8` limits CPU time, not a dedicated set of physical cores.

Each container is limited to 8 GiB RAM with no additional swap allowance.
Limits are not reservations. Four containers can request 32 GiB in total, so
`st`'s approximately 31 GiB RAM is overcommitted before accounting for its other
services. Concurrent heavy builds may encounter memory pressure or OOM; monitor
peak usage and adjust only with the operator's approval.

Jobs must keep separate workspaces, process namespaces and runtime directories.
Do not add shared writable build-directory mounts, host networking, host PID
namespaces, fixed host-port mappings or host desktop/audio sockets. Identical
in-container socket paths are safe with the existing isolation.

Before changing the live runner, back up its configuration and wait for running
jobs to finish. Restart only the DeskWaifu CI runner, not the separate review
runners. Check service health and inspect a newly created container's CPU and
memory cgroup limits after applying changes.
