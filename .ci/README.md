<!--
Copyright 2026, Mateo de Mayo
SPDX-License-Identifier: BSD-3-Clause

Author: Mateo de Mayo <mateo.demayo@tum.de>
-->

# CI (Single Docker Runner) Setup

This repository’s CI is designed to run **builds, evaluations, timing evaluations, and report generation** on a **single GitLab runner** using the **Docker executor**.

The main motivation is to allow running experiments on a workstation that already has ~1TB of SLAM datasets pre-downloaded, while avoiding giving users arbitrary shell access to the host.

## Overview

The CI has two layers:

1. **Main pipeline** (`.gitlab-ci.yml`) builds packages and can trigger an evaluation pipeline.
2. **Generated evaluation pipeline** (`.ci/cieval.yaml`) is produced by `.ci/cievalgen.py` from `.ci/cieval.template.yaml` + `.ci/evaluation.json`.

The evaluation pipeline:
- Builds `build/basalt_vio`
- Runs datasets (many jobs in parallel)
- Produces `results.zip` and `timing-results.zip`

The main pipeline:
- Has a `create-evaluation` job that generates `.ci/cieval.yaml`
- Has `do-evaluation` which triggers the downstream evaluation pipeline
- Has `get-results` and `report` which fetch results from the downstream pipeline and render a report

## What you need on the host

### 1) Install and register a GitLab runner

- Install `gitlab-runner` on the workstation.
- Ensure Docker works for the user that runs `gitlab-runner` (or use root-owned runner with a root-owned Docker daemon).
- Register **one runner** with **executor = docker** and add a single tag: `basalt-evaluation-box`

*NOTE: the `basalt-evaluation-box` tag is used by jobs that expect a custom runner with
local tools and datasets set up as described below.*

### 2) Put datasets somewhere stable

The runner will bind-mount datasets into the container. You must have:
- EuRoC dataset directory
- TUMVI dataset directory
- Monado SLAM datasets directory (with subfolders for devices like Odyssey+, Valve Index, Reverb G2)

### 3) Have xrtslam-metrics available as a mounted directory

Report generation calls scripts from `xrtslam-metrics` via `/xrtmet/...` and expects targets under `/xrtmet/test/data/targets`.

You should mount the `xrtslam-metrics` repo into containers and run it from there (recommended), rather than installing it globally on the host.

Furthermore you need to craft a `xrtslam-metrics/.venv-docker` virtual environment that can be sourced by the CI:

```bash
docker run -v /your/host/path/to/xrtslam-metrics/:/xrtmet:rw --rm -it registry.freedesktop.org/mateosss/basalt:ubuntu2404 bash
cd /xrtmet/
python3.12 -m venv .venv-docker
source .venv-docker/bin/activate
pip install poetry
poetry update
```

## Runner `config.toml` (single runner)

Edit `.gitlab-runner/config.toml` and configure a single Docker runner.

Example (adapt paths to your machine):

```toml
concurrent = 5
check_interval = 0

[[runners]]
  name = "basalt-single-docker-runner"
  url = "https://gitlab.freedesktop.org"
  token = "REDACTED"
  executor = "docker"

  [runners.docker]
    image = "registry.freedesktop.org/mateosss/basalt:ubuntu2404"
    privileged = false
    disable_cache = false
    shm_size = 0

    # Bind mounts: datasets (read-only), xrtmet repo (read-only),
    # and writable locations for cache/ccache.
    volumes = [
      # GitLab cache volume (used with "cache" yaml keyword)
      # NOTE: Currently unused to avoid possible cache compression delays.
      "/storage/local/ssd/<user>/docker-cache:/cache",

      # We store ccache cache here
      "/storage/local/ssd/<user>/ccache:/ccache",

      # We use set a scratchdir from the host filesystem (in a fast disk)
      # because extracting big files into the docker filesystem is much slower.
      "/storage/local/ssd/<user>/scratch:/scratch:rw",

      # xrtslam-metrics repo (read-only)
      "/storage/local/ssd/<user>/xrtslam-metrics:/xrtmet:ro",

      # Datasets (read-only)
      "/storage/local/ssd/<user>/euroc:/euroc:ro",
      "/storage/local/ssd/<user>/tumvi:/tumvi:ro",
      "/storage/local/hdd/monado-slam-datasets/M_monado_datasets/MO_odyssey_plus:/msdmo:ro",
      "/storage/local/hdd/monado-slam-datasets/M_monado_datasets/MI_valve_index:/msdmi:ro",
      "/storage/local/hdd/monado-slam-datasets/M_monado_datasets/MG_reverb_g2:/msdmg:ro"
    ]
