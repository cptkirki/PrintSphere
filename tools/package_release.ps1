param(
    [string]$Version = "v1.6.1"
)

$ErrorActionPreference = "Stop"

$variants = @(
    @{
        Name = "amoled_1_75"
        BuildDir = "build-amoled_1_75"
        ReleaseRoot = "release"
        Version = $Version
    },
    @{
        Name = "lcd_2_8c"
        BuildDir = "build-lcd_2_8c"
        ReleaseRoot = "release/2.8c"
        Version = "$Version-2.8c"
    },
    @{
        Name = "knomi_v2"
        BuildDir = "build-knomi_v2"
        ReleaseRoot = "release/knomi_v2"
        Version = "$Version-knomi_v2"
    }
)

foreach ($variant in $variants) {
    $buildDir = Join-Path $PSScriptRoot "..\$($variant.BuildDir)"
    $releaseRoot = Join-Path $PSScriptRoot "..\$($variant.ReleaseRoot)"
    $packager = Join-Path $PSScriptRoot "package_initial_flash.py"

    if (-not (Test-Path $buildDir)) {
        throw "Build-Ordner fehlt fuer $($variant.Name): $buildDir"
    }

    if (-not (Test-Path (Join-Path $buildDir "flasher_args.json"))) {
        throw "flasher_args.json fehlt fuer $($variant.Name): $buildDir"
    }

    Write-Host ""
    Write-Host "Packaging $($variant.Name) aus $($variant.BuildDir) -> $($variant.ReleaseRoot)" -ForegroundColor Cyan

    & python $packager `
        --build-dir $buildDir `
        --release-root $releaseRoot `
        --version $variant.Version

    if ($LASTEXITCODE -ne 0) {
        throw "Packaging fehlgeschlagen fuer $($variant.Name)"
    }
}

Write-Host ""
Write-Host "Packaging fuer beide Hardware-Varianten abgeschlossen." -ForegroundColor Green
