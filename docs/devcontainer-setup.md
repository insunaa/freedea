# Devcontainer Setup & Rebuild Checklist

The devcontainer is **clone-and-go**: creating the container from this repo
builds an image (`.devcontainer/Dockerfile`) that already contains every
dependency — toolchain, PlatformIO, clang-format, the manual-PDF and icon
regeneration tool sets — and `devcontainer.json`'s `postCreateCommand`
initializes the git submodule. `DEPENDENCIES.md` (repo root) is the source of
truth for the dependency pins.

Only `/workspaces` (and thus this repo, including the gitignored `tmp/` assets)
survives a container rebuild; everything else — apt packages, `~/.platformio` —
is part of the image and comes back automatically, except the pio-managed
platform downloads (see §3).

The container runs as **`ubuntu` (uid 1000)** — set explicitly via
`containerUser`/`remoteUser` in `devcontainer.json` and by the Dockerfile's
default `USER` — the only uid with write access to a bind-mounted
`/dev/ttyACM0` (§7). Passwordless `sudo` is available (image sudoers drop-in
`/etc/sudoers.d/90-ubuntu`). The repo worktree is owned by uid 1000, so git
needs no `safe.directory` config.

## 1. apt packages

All baked into `.devcontainer/Dockerfile` (`sudo git curl build-essential
python3-venv python3-pip clang-format libmbedtls-dev pandoc weasyprint
poppler-utils python3-pil librsvg2-bin`) — nothing to do on a rebuild.
Manual fallback if a package is missing:

```bash
sudo apt-get update && sudo apt-get install -y <package>
```

- `clang-format` must be **21.1.8** to match the CI pin; Ubuntu 26.04 ships
  exactly that — verify with `clang-format --version`.
- `libmbedtls-dev` provides `-lmbedcrypto`, linked by the Vault and MideaAC
  host tests.
- `g++` (from `build-essential`) is required by `lib/MideaAC/test/host/run.sh`.

## 2. PlatformIO

Baked into the image: PlatformIO **6.1.19** (pinned) lives in a venv at
`/opt/platformio` with `pio` symlinked into `PATH`; nothing to install. (The
official `get-platformio.py` installer crashes on Python 3.14, which is why the
venv route is used and the version pinned.)

```bash
pio --version   # expect 6.1.19
```

## 3. First build (re-downloads the pio platform)

`pio run -e x4` fetches the pioarduino platform (55.03.37),
`toolchain-riscv32-esp`, the Arduino framework, and the platform's `penv` —
all into `~/.platformio` (`/home/ubuntu/.platformio`), which is **not** part of
the image and not under `/workspaces`. Expect several minutes on a fresh
container; don't interrupt.

If it dies with `ModuleNotFoundError: No module named 'littlefs'`, the platform's
penv provisioning skipped its deps (no-internet probe): run

```bash
~/.platformio/penv/bin/python -m pip install "littlefs-python>=0.16.0" "fatfs-ng>=0.1.14"
```

## 4. Git & submodules

`postCreateCommand` runs `git submodule update --init --recursive` on every
container create (idempotent when already initialized). Repo-local identity
(`freedea <freedea@freedea.local>`) survives in `.git/config`, and the running
user (uid 1000) owns the worktree, so the old root-user `safe.directory`
workaround is not needed.

## 5. Verify

```bash
pio --version                              # 6.1.19
clang-format --version                     # 21.1.8
pio run -e x4                              # build green (current baseline ≈86.6 KB RAM / ≈1.16 MB flash; see RELEASE_PLAN R.4 for final numbers)
for t in lib/*/test/host/run.sh; do sh "$t" || exit 1; done   # 7× ALL TESTS PASSED
bin/build_manual_pdf.sh                    # renders docs/Freedea-User-Manual.pdf
```

## 6. Reference assets (all optional)

`tmp/` (gitignored) holds disposable development references only: the
`midea-msmart` clone, reference clones of `freeink-sdk`/`wakeink`, and serial
captures. Every regeneration input the project needs is **tracked** — staged
license sources in `tools/licenses/`, the vector extractor at
`lib/MideaAC/test/host/vectors/extract_vectors.py` (msmart clone arg, defaults
to `tmp/midea-msmart`), and the pinned Lucide icon recipe in `src/ui/icons.txt`.
Never distribute `tmp/`.

## 7. Device flashing (opt-in mount)

`devcontainer.json` does **not** mount `/dev/ttyACM0` by default, so the
container also starts on machines with no X4 attached. To flash, uncomment the
commented `mounts` block in `devcontainer.json` and rebuild the container.
Then:

```bash
pio run -e x4 -t upload    # add -p /dev/ttyACM0 if auto-detect fails
pio device monitor         # 115200 baud
```
