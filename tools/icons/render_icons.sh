#!/usr/bin/env bash
# Renders the logo SVG to the PNG sizes the runtime loads (the runtime has no
# SVG renderer). The SVG's "BFR" is text in IBM Plex Sans Bold Italic, so run
# this where that font is installed, or the letters fall back to another face.
set -euo pipefail
cd "$(dirname "$0")/../../runtime/resources/icons"
fonts=$(fc-list)
grep -qi "IBM Plex Sans.*Bold Italic" <<<"$fonts" || { echo "IBM Plex Sans Bold Italic is not installed" >&2; exit 1; }
for size in 128 48; do
    rsvg-convert -w "$size" -h "$size" bfr_logo.svg -o "bfr_logo_${size}.png"
done
ls -la bfr_logo_*.png
