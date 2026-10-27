# DeskWaifu CI runner

## Operational target (not verified live)

The documented deployment uses the `deskwaifu-ci-runner.service` user service on
`st` to run container jobs through rootless Podman, with configuration at
`~/.config/deskwaifu-ci-runner/config.yaml`. The intended limits are:

```yaml
runner:
  capacity: 4
container:
  options: --cpus=8 --memory=8g --memory-swap=8g
```

These are excerpts, not a replacement configuration. Preserve the registered
runner identity, image-ID-pinned `deskwaifu-arch` label, `force_pull: false`,
`docker_host: "-"`, `privileged: false`, and `valid_volumes: []`.

The repository does not contain the deployed service unit or full runner
configuration. The live service, rootless execution, limits, label mapping and
other settings above have not been verified here.

With these limits applied, each container has an 8-CPU time quota, not a
dedicated set of physical cores, and 8 GiB RAM with no additional swap allowance.
Limits are not reservations. Four containers have a combined 32 GiB memory
limit. The previously documented approximately 31 GiB of RAM on `st` has not
been verified here; if accurate, those limits overcommit RAM before accounting
for other services. Concurrent heavy builds may encounter memory pressure or
OOM; monitor peak usage and adjust only with the operator's approval.

## Repository-backed behavior

`.gitea/workflows/ci.yml` selects `deskwaifu-arch` and requires an image-ID-pinned
image with `force_pull: false` in its comments; it does not configure the runner.
The main build, render-target variant builds and sanitizer build explicitly use
`cmake --build ... --parallel 16`. Do not derive compiler parallelism from
`nproc`: this job count intentionally exceeds the intended 8-CPU quota.

`scripts/rebuild-ci-image.sh` builds the `ci` target from `Containerfile`, checks
the image and privately transfers it to `${DESKWAIFU_RUNNER_HOST:-st}`. The image
contains proprietary dependencies and must remain private. The script checks
that the image ID exists on the destination and prints the
`deskwaifu-arch:docker://sha256:...` label; it does not update the runner
configuration or restart the service. Apply that label manually with
`force_pull: false` when updating the runner.

## Isolation and live verification

Jobs must keep separate workspaces, process namespaces and runtime directories.
Do not add shared writable build-directory mounts, host networking, host PID
namespaces, fixed host-port mappings or host desktop/audio sockets. Identical
in-container socket paths are safe only when the required isolation holds.

The CI isolation check only verifies that `/var/run/docker.sock` is not a socket.
`scripts/test-environment.py` creates private runtime directories and a D-Bus
session for tests it launches; it is not a network/filesystem security sandbox.
These checks do not establish container workspace, mount, network or PID
isolation. Verify those properties against the live runner and containers.

Before changing the live runner, back up its configuration and wait for running
jobs to finish. The review workflow selects the distinct `opencode-review` label;
this does not prove that its deployed service is separate. Confirm service
boundaries before restarting only the DeskWaifu CI runner, not review runners.
Check service health, the applied image ID and isolation settings, and a newly
created container's CPU and memory cgroup limits after applying changes.
