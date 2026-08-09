# Notice / Attribution

This repository is a **derivative** of [PrintSphere](https://github.com/cptkirki/PrintSphere)
by [cptkirki](https://github.com/cptkirki) (Cpt_Kirk).

Upstream project:
- Source: https://github.com/cptkirki/PrintSphere
- License: Federation Non-Commercial License (FNCL) v1.1 (see `LICENSE`)
- Copyright (c) 2026 Cpt_Kirk

## Changes in this fork

Relative to upstream PrintSphere, this tree adds or extends:

- BigTreeTech **Knomi v2** hardware support (GC9A01 + CST816S BSP, board config, build variant)
- Knomi-oriented UI layout (compact AMS, preview, fonts, humidity pill)
- Display rotation via GC9A01 MADCTL (SPI / `OTHER` panel path)
- HMS / error-lookup fix for UTF-8 BOM in the embedded TSV
- Packaging and flash paths for the `knomi_v2` release variant

Unmodified upstream behavior for Waveshare AMOLED 1.75 and LCD 2.8C is intended to remain available via the existing `PRINTSPHERE_HW_VARIANT` builds.

## License obligations

When you share this software (modified or not), the FNCL requires that you:

1. Keep the `LICENSE` text and copyright notice.
2. Clearly mark that the software was changed and briefly describe the changes (this file).

Commercial use requires a separate license from the copyright holder.
