# Compatibility entry point; keep one authoritative, drift-checked patcher.
param([string]$AdapterRoot = "managed_components/espressif__esp_lvgl_adapter")
$ErrorActionPreference = 'Stop'
$patcher = Join-Path $PSScriptRoot 'apply_adapter_patches.py'
if ($env:IDF_PYTHON_ENV_PATH) {
    $python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe'
} else {
    $python = 'python'
}
& $python $patcher --root $AdapterRoot
exit $LASTEXITCODE
