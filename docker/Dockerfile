# syntax=docker/dockerfile:1
# Freedea firmware build — reproducible, host-toolchain-free.
#
# Normal entry point: bin/build_docker.sh / bin/build_docker.ps1 (they build
# the export target, extract the .bin, and name it dist/freedea-x4-<ver>.bin).
# Manual use:
#   docker build -f docker/Dockerfile --build-arg PIO_ENV=x4_release .
#
# Submodules arrive via the build context (working trees are copied; .git is
# excluded by .dockerignore), so run `git submodule update --init --recursive`
# before building — the scripts preflight this.

# PIO_ENV: PlatformIO environment to build (x4 = dev/debug, x4_release = ship).
ARG PIO_ENV=x4_release

FROM ubuntu:26.04 AS build

# Firmware-build dependency set only (DEPENDENCIES.md §A, no manual-PDF or
# icon-regeneration tooling). ca-certificates for the platform/framework
# downloads; git for pio's occasional git fetches.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      git \
      curl \
      build-essential \
      python3-venv \
      python3-pip \
    && rm -rf /var/lib/apt/lists/*

# PlatformIO pinned from the repo-root versions.env (single source of truth).
COPY versions.env /tmp/pins.env
RUN . /tmp/pins.env \
    && python3 -m venv /opt/platformio \
    && /opt/platformio/bin/pip install --no-cache-dir "platformio==${PLATFORMIO_VERSION}" \
    && ln -sf /opt/platformio/bin/pio /usr/local/bin/pio \
    && rm /tmp/pins.env

COPY --chown=ubuntu:ubuntu . /src
WORKDIR /src

ARG PIO_ENV
USER ubuntu
RUN pio run -e "${PIO_ENV}"

# Artifact carrier: the image's only file is the firmware binary. Extracted
# with `create` + `cp` + `rm` by the build scripts (works on docker+podman
# without BuildKit --output support).
FROM scratch AS export
ARG PIO_ENV
COPY --from=build /src/.pio/build/${PIO_ENV}/firmware.bin /freedea.bin
