FROM ubuntu:26.04

# Version pins come from the repo-root versions.env (single source of truth;
# the devcontainer build context is the repo root, see devcontainer.json).
COPY versions.env /tmp/pins.env

# Full dependency set per DEPENDENCIES.md (docs for rationale). curl covers SDK
# doc fetches; clang-format matches the CI pin from .env (the apt version check
# below fails the build if Ubuntu 26.04 ever ships a different one);
# libmbedtls-dev links the software-AES host tests (Vault, MideaAC).
# pandoc/weasyprint/poppler-utils build the user manual PDF
# (bin/build_manual_pdf.sh; fonts-noto-core provides its Noto Sans fonts); python3-pil + librsvg2-bin regenerate the control
# icons (src/ui/icons.txt); python3-serial backs standalone capture scripts
# against the X4 console (pio device monitor uses the PlatformIO venv's own).
RUN apt-get update && apt-get install -y \
      sudo \
      git \
      curl \
      build-essential \
      python3-venv \
      python3-pip \
      clang-format \
      libmbedtls-dev \
      pandoc \
      weasyprint \
      poppler-utils \
      fonts-noto-core \
      python3-pil \
      librsvg2-bin \
      python3-serial \
    && rm -rf /var/lib/apt/lists/*

# Fail fast if the apt clang-format drifts from the CI pin in .env.
RUN . /tmp/pins.env \
    && clang-format --version | grep -q "$CLANG_FORMAT_VERSION"

# PlatformIO pinned from .env into a venv (the official get-platformio.py
# installer is broken on Python 3.14 — venv route). Owned by ubuntu so later
# `pio` invocations never need sudo; pio itself installs packages into
# ~/.platformio, not into the venv.
RUN . /tmp/pins.env \
    && python3 -m venv /opt/platformio \
    && /opt/platformio/bin/pip install --no-cache-dir "platformio==${PLATFORMIO_VERSION}" \
    && chown -R ubuntu:ubuntu /opt/platformio \
    && ln -sf /opt/platformio/bin/pio /usr/local/bin/pio \
    && rm /tmp/pins.env

# The container runs as the stock `ubuntu` user (uid 1000): it is the only uid
# the container runtime grants write access to the /dev/ttyACM0 X4 serial
# device. Passwordless sudo (for apt and other rare root tasks) via drop-in.
RUN echo 'ubuntu ALL=(ALL:ALL) NOPASSWD:ALL' > /etc/sudoers.d/90-ubuntu \
    && chmod 0440 /etc/sudoers.d/90-ubuntu \
    && visudo -c

USER ubuntu
WORKDIR /workspaces
