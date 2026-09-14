# Build the Freedea firmware inside a container (docker or podman) and place
# the binary in dist/freedea-x4-<version>.bin. No local toolchain required.
#
# Usage:  .\bin\build_docker.ps1 [-PioEnv <pio-env>] [-Tool docker|podman] [-NoCache]
# Requires: git, docker OR podman. Submodules must be initialized.

[CmdletBinding()]
param(
    # NOTE: not named $Env — that collides with PowerShell's ${Env:} scope.
    [string]$PioEnv = "x4_release",
    [string]$Tool = $env:CONTAINER_TOOL,
    [switch]$NoCache
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

if (-not $Tool) {
    foreach ($candidate in @("docker", "podman")) {
        if (Get-Command $candidate -ErrorAction SilentlyContinue) { $Tool = $candidate; break }
    }
}
if (-not $Tool) {
    throw "No container tool found (install docker or podman, or set CONTAINER_TOOL)."
}

# Submodule preflight: an uninitialized entry shows a leading '-' in the status.
$submodules = & git -C $Root submodule status --recursive 2>$null
if ($submodules -match '^-') {
    throw "Submodules are not initialized. Run: git submodule update --init --recursive"
}

$versionLine = Select-String -Path (Join-Path $Root "src/AppVersion.h") -Pattern 'kAppVersion\s*=\s*"([^"]+)"' |
    Select-Object -First 1
if (-not $versionLine) { throw "Could not parse kAppVersion from src/AppVersion.h." }
$version = $versionLine.Matches[0].Groups[1].Value

if ($PioEnv -eq "x4_release") {
    $Out = Join-Path $Root "dist/freedea-x4-$version.bin"
} else {
    $Out = Join-Path $Root "dist/freedea-$PioEnv-$version.bin"
}
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null

# Single untagged build straight to the artifact-carrier (export) stage — the
# build stages run as its dependencies. The image exists only to hand out
# /freedea.bin: --iidfile captures its ID (docker+podman), and after the copy
# both container and image are deleted; only the build layers stay in the
# local cache. --platform: Apple-Silicon hosts build the amd64 image
# transparently.
$buildArgs = @("build")
if ($NoCache) { $buildArgs += "--no-cache" }
$iidFile = New-TemporaryFile
$buildArgs += @("--platform", "linux/amd64", "-f", (Join-Path $Root "docker/Dockerfile"),
    "--target", "export", "--iidfile", $iidFile.Path, "--build-arg", "PIO_ENV=$PioEnv")

try {
    & $Tool @buildArgs $Root
    if ($LASTEXITCODE -ne 0) { throw "Image build failed." }

    $iid = (Get-Content $iidFile.Path -Raw).Trim()
    if (-not $iid) { throw "Build produced no image ID." }

    $cid = & $Tool create $iid
    if ($LASTEXITCODE -ne 0) { throw "Container create failed." }
    try {
        & $Tool cp "${cid}:/freedea.bin" $Out
        if ($LASTEXITCODE -ne 0) { throw "Artifact copy failed." }
        Write-Host "Wrote $Out"
        Write-Host ((Get-FileHash -Algorithm SHA256 $Out).Hash.ToLower() + "  $Out")
    } finally {
        & $Tool rm $cid 2>$null | Out-Null
        & $Tool rmi $iid 2>$null | Out-Null
    }
} finally {
    Remove-Item $iidFile.Path -Force -ErrorAction SilentlyContinue
}
