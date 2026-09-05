# Regenerates LVGL Dosis/Montserrat C fonts with ASCII punctuation and German
# umlauts (Ä Ö Ü ß ä ö ü) plus the degree sign.
# Requires: Node.js (npx lv_font_conv), Python 3 with fonttools.

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path $env:TEMP "printsphere-fontgen"
New-Item -ItemType Directory -Force -Path $work | Out-Null
Set-Location $work

python -m pip install --quiet fonttools
npm install --no-fund --no-audit lv_font_conv | Out-Null

curl.exe -L --fail -o "Dosis-variable.ttf" "https://github.com/google/fonts/raw/main/ofl/dosis/Dosis%5Bwght%5D.ttf"
curl.exe -L --fail -o "Montserrat-Medium.ttf" "https://raw.githubusercontent.com/lvgl/lvgl/v9.5.0/scripts/built_in_font/Montserrat-Medium.ttf"

python -c @"
from fontTools.varLib import instancer
from fontTools.ttLib import TTFont
font = TTFont('Dosis-variable.ttf')
instancer.instantiateVariableFont(font, {'wght': 400}).save('Dosis-Regular.ttf')
"@

$range = "0x20-0x7E,0xB0,0xC4,0xD6,0xDC,0xDF,0xE4,0xF6,0xFC"
$outDir = Join-Path $root "main\include\font"

function Convert-LvFont([string]$Font, [int]$Size, [string]$Name) {
    npx --no-install lv_font_conv `
        --font $Font --size $Size --bpp 4 --format lvgl --no-compress `
        --force-fast-kern-format --lv-include lvgl.h --lv-font-name $Name `
        --range $range -o (Join-Path $outDir "$Name.c")
}

Convert-LvFont "Dosis-Regular.ttf" 20 "dosis_20"
Convert-LvFont "Dosis-Regular.ttf" 32 "dosis_32"
Convert-LvFont "Dosis-Regular.ttf" 40 "dosis_40"
Convert-LvFont "Montserrat-Medium.ttf" 12 "montserrat_12"
Convert-LvFont "Montserrat-Medium.ttf" 14 "montserrat_14"
Convert-LvFont "Montserrat-Medium.ttf" 20 "montserrat_20"

python -c @"
from pathlib import Path
needle = '    .dsc = &font_dsc,'
insert = '    .static_bitmap = 0,\n    .dsc = &font_dsc,'
root = Path(r'$outDir')
for p in root.glob('*.c'):
    if not (p.name.startswith('dosis_') or p.name.startswith('montserrat_')):
        continue
    text = p.read_text(encoding='utf-8')
    if '.static_bitmap' in text:
        continue
    last = text.rfind(needle)
    if last >= 0:
        p.write_text(text[:last] + insert + text[last+len(needle):], encoding='utf-8')
"@

Write-Host "Wrote Latin fonts to $outDir"
