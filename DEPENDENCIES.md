# Freedea — Development Dependencies

Declarative manifest of every external dependency, per task. The
machine-readable pin list is **`versions.env`** (repo root, sourced by both
Dockerfiles, `bin/build_docker.sh`/`.ps1` and the CI workflows); this file is
the human rationale. `.devcontainer/Dockerfile` (full set) and
`docker/Dockerfile` (firmware-build set only) COPY `versions.env` and install
from it. When adding a tool, update `versions.env`, this file and both
Dockerfiles in the same change.

Pinned in `versions.env`: PlatformIO `6.1.19`, clang-format `21.1.8`, platform
`pioarduino 55.03.37`, ArduinoJson `7.4.2`, FreeInk SDK submodule @ `e0fcdb1`,
msmart vectors @ `d7db53bc47`, Lucide icons @ `a79b2d1` (each pinned so CI,
devcontainer, and regenerated artifacts agree). CI `build.yml` fails on drift
against the pins that must stay inline in `platformio.ini` (PlatformIO INI has
no file substitution) and asserts the FreeInk SDK gitlink equals
`FREEINK_SDK_REF`.

## A. Firmware build

Consumers: `.devcontainer` (all), `docker/Dockerfile` (all), CI `build.yml`
(installs pio itself on the GitHub runner).

| Dependency | Version / pin | Provided by | Verify |
| --- | --- | --- | --- |
| OS base | ubuntu 26.04 | Dockerfile `FROM` | `lsb_release -ds` |
| git, curl | apt | apt | `git --version` |
| build-essential (g++) | apt | apt | `g++ --version` |
| python3-venv, python3-pip | apt | apt | `python3 -m venv --help` |
| PlatformIO | **6.1.19** (`PLATFORMIO_VERSION`), venv at `/opt/platformio`, `pio` symlinked into PATH | Dockerfile, from `versions.env` (official installer is broken on Python 3.14 — venv route) | `pio --version` |
| FreeInk SDK submodule | gitlink `vendor/freeink-sdk` @ `e0fcdb1…` (authoritative; mirrored as `FREEINK_SDK_REF` in `versions.env`, CI-enforced — `.gitmodules` only stores path+URL, never the SHA) | `git submodule update --init --recursive` | `git rev-parse HEAD:vendor/freeink-sdk` |
| pio-managed toolchain | platform `pioarduino 55.03.37`, `toolchain-riscv32-esp`, Arduino framework 3.3.7 core, penv (`littlefs-python>=0.16.0`, `fatfs-ng>=0.1.14`) | auto-downloaded by first `pio run` into `~/.platformio` (~7 GB) | `pio run -e x4` |
| ArduinoJson | 7.4.2 | `lib_deps` (pio-managed) | — |

Removed from the image 2026-09-13 (RELEASE_PLAN R.2): `libssl-dev` +
`pkg-config` had no consumer — only `-lmbedcrypto` appears in test runners,
never `-lssl`/`-lcrypto`.

## B. Host unit tests (`lib/*/test/host/run.sh` × 7)

Consumers: `.devcontainer` (all), `docker/Dockerfile` (optional test stage).

| Dependency | Version / pin | Provided by | Verify |
| --- | --- | --- | --- |
| g++ (C++20) | apt build-essential | apt | run `sh lib/MideaAC/test/host/run.sh` |
| **libmbedtls-dev** | apt (`-lmbedcrypto`: Vault + MideaAC software AES backends) | apt (added 2026-09-13 — was missing; fresh containers could not run the tests) | `sh lib/Vault/test/host/run.sh` |

## C. Code quality

| Dependency | Version / pin | Provided by | Verify |
| --- | --- | --- | --- |
| clang-format | **21.1.8** (`CLANG_FORMAT_VERSION`; Ubuntu 26.04 apt ships exactly this — the devcontainer Dockerfile fails the build on drift; CI pins via pip) | apt / CI `pip` | `clang-format --version` |

Only ever used through `./bin/clang-format-fix` (plain `sh` + native
clang-format; formats tracked git files only).

## D. User manual PDF (`bin/build_manual_pdf.sh`)

Consumers: `.devcontainer` (all). **Not** needed by `docker/Dockerfile` (the
manual is not part of the firmware build).

| Dependency | Version / pin | Provided by | Verify |
| --- | --- | --- | --- |
| pandoc | apt (3.7) | apt (added 2026-09-13) | `pandoc --version` |
| weasyprint | apt (67) — PDF engine; no LaTeX, no Node.js | apt (added 2026-09-13) | `weasyprint --version` |
| poppler-utils | apt (`pdfinfo`/`pdftoppm`, output sanity-check only) | apt (added 2026-09-13) | `pdfinfo -v` |

## E. Regeneration flows (optional dev tools; outputs are committed —
regeneration never runs in CI or in the Docker build)

| Flow | Inputs | Tools | Input durability |
| --- | --- | --- | --- |
| On-device license texts — `bin/gen_license_texts.py` → `src/ui/LicenseTexts.h` | **staged license sources** (7 files: mbedtls, esp-idf, arduino, freeink, lwip-init.c, midea-msmart, lucide), tracked at `tools/licenses/` | python3 stdlib only | durable (R.6) |
| Control-row icons — `vendor/freeink-sdk/.../tools/gen_icons.py` → `src/ui/icons_gen.h` | Lucide repo clone @ **a79b2d1** (re-fetch documented in `src/ui/icons.txt`), manifest `src/ui/icons.txt` | `librsvg2-bin`, `python3-pil` | fully reproducible from the network — nothing to preserve |
| msmart golden vectors — → `lib/MideaAC/test/host/vectors/` | `extract_vectors.py` (tracked next to the vectors), midea-msmart clone @ **d7db53b…** (ref in `versions.env`), python venv with `pycryptodome`, `httpx` | python3 venv | durable (R.6); clone itself is a disposable input |
| Manual PDF | see D | see D | docs in git |

There is **no font-generation step** in this repo (fonts ship prebuilt inside
the vendored FreeInk SDK).

## F. Containerized release build (`bin/build_docker.sh` / `.ps1`)

Host-level, optional: `docker` **or** `podman` (plus `git` for the submodule
preflight). The image itself installs section A's set. `CONTAINER_TOOL=docker|podman`
overrides auto-detection.

## G. Device flashing & serial debug

Consumers: `.devcontainer` only (flashing/monitoring a USB-attached X4).
**Not** in `docker/Dockerfile` — no device is present during a build.

| Dependency | Version / pin | Provided by | Verify |
| --- | --- | --- | --- |
| esptool (flashing) | pio-managed, via `pio run -t upload` | PlatformIO (section A) | `pio run -e x4 -t upload` |
| pyserial | apt `python3-serial` (3.5) — backs `bin/capture_serial.py` (timed console capture, optional RTS reset pulse; `--no-reset` for passive reads) | apt (added 2026-09-13; previously an unpersisted `pip --break-system-packages` install) | `bin/capture_serial.py /dev/ttyACM0 3 --no-reset` |
| Serial device access | runtime concern, not a package: the devcontainer runs as uid 1000, the uid the runtime grants rw on `/dev/ttyACM0` | `.devcontainer/Dockerfile` (comment) | `ls -l /dev/ttyACM0` |

`pio device monitor` needs nothing extra — it uses the pyserial inside the
PlatformIO venv.

## Not needed (common assumptions to avoid)

- Node.js / npm — prohibited by project rules.
- ESP-IDF separately — comes through pioarduino/platformio.
- cmake — unused by this project.
- LaTeX — the manual pipeline is pandoc → weasyprint.
- OpenSSL dev headers — no consumer left (see A).
