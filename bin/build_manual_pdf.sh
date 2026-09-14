#!/usr/bin/env bash
# Generate the Freedea user manual PDF from docs/USER_MANUAL.md.
#
# Usage:  bin/build_manual_pdf.sh [input.md] [output.pdf]
#         (defaults: docs/USER_MANUAL.md -> docs/Freedea-User-Manual.pdf)
#
# Dependencies (Ubuntu/Debian):  sudo apt install pandoc weasyprint
# Pandoc converts Markdown -> styled HTML, WeasyPrint renders the PDF (no
# LaTeX, no Node.js).
set -euo pipefail

# Deterministic text encoding regardless of the caller's locale; pandoc
# mangles non-ASCII metadata under a C locale.
export LANG=C.UTF-8 LC_ALL=C.UTF-8

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INPUT="${1:-"$ROOT/docs/USER_MANUAL.md"}"
OUTPUT="${2:-"$ROOT/docs/Freedea-User-Manual.pdf"}"

for tool in pandoc weasyprint; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: '$tool' not found. Install with: sudo apt install pandoc weasyprint" >&2
    exit 1
  fi
done

[ -f "$INPUT" ] || { echo "error: input not found: $INPUT" >&2; exit 1; }

# Footer version comes from the single source of truth (src/AppVersion.h).
VERSION="$(sed -n 's/.*kAppVersion *= *"\([^"]*\)".*/\1/p' "$ROOT/src/AppVersion.h")"
[ -n "$VERSION" ] || { echo "error: could not parse kAppVersion from src/AppVersion.h" >&2; exit 1; }
DATE="$(date +%Y-%m-%d)"
CSS_FILE="$(mktemp /tmp/freedea-manual-styles-XXXXXX.css)"
trap 'rm -f "$CSS_FILE"' EXIT

cat >"$CSS_FILE" <<CSS
@page {
  size: A4;
  margin: 22mm 18mm 20mm 18mm;
  @bottom-left {
    content: "Freedea v${VERSION} User Manual";
    font-family: "Noto Sans", sans-serif;
    font-size: 7.5pt;
    color: #777;
  }
  @bottom-right {
    content: counter(page) " / " counter(pages);
    font-family: "Noto Sans", sans-serif;
    font-size: 7.5pt;
    color: #777;
  }
}
body {
  font-family: "Noto Sans", sans-serif;
  font-size: 9.5pt;
  line-height: 1.45;
  color: #111;
  hyphens: auto;
}
h1 { font-size: 19pt; margin: 0 0 4pt 0; }
h2 {
  font-size: 13.5pt;
  margin: 20pt 0 6pt 0;
  padding-bottom: 2pt;
  border-bottom: 1.2pt solid #333;
  break-after: avoid;
}
h3 { font-size: 11pt; margin: 12pt 0 4pt 0; break-after: avoid; }
p, li { text-align: justify; }
hr { border: none; border-top: 0.6pt solid #bbb; margin: 10pt 0; }
table {
  border-collapse: collapse;
  margin: 8pt 0;
  width: 100%;
  break-inside: avoid;
}
th, td {
  border: 0.6pt solid #999;
  padding: 3pt 6pt;
  vertical-align: top;
  text-align: left;
}
th { background: #eee; }
code {
  font-family: "Noto Sans Mono", monospace;
  font-size: 8.5pt;
  background: #f2f2f2;
  padding: 0 2pt;
}
pre {
  background: #f2f2f2;
  padding: 6pt 8pt;
  border-left: 2pt solid #999;
  break-inside: avoid;
  white-space: pre-wrap;
}
pre code { background: none; padding: 0; }
blockquote {
  margin: 8pt 0;
  padding: 4pt 10pt;
  border-left: 2pt solid #666;
  background: #f7f7f7;
  break-inside: avoid;
}
header#title-block-header {
  margin: 0 0 14pt 0;
  padding-bottom: 8pt;
  border-bottom: 2pt solid #333;
}
header#title-block-header h1.title { font-size: 24pt; margin: 0; }
header#title-block-header p.subtitle { color: #444; font-size: 11pt; margin: 4pt 0 0 0; }
header#title-block-header p.date { color: #777; font-size: 9pt; margin: 2pt 0 0 0; }
nav#TOC h2 { border-bottom: none; margin-top: 4pt; }
nav#TOC { break-after: page; font-size: 9.5pt; }
nav#TOC > ul { list-style: none; padding-left: 0; }
nav#TOC ul ul { list-style: none; padding-left: 14pt; }
nav#TOC li { margin: 2pt 0; }
nav#TOC a { text-decoration: none; color: #111; }
/* WeasyPrint resolves real page numbers for the TOC entries. */
nav#TOC a::after {
  content: leader('.') target-counter(attr(href), page);
  color: #666;
}
CSS

pandoc "$INPUT" \
  --standalone \
  --toc --toc-depth=2 \
  --metadata title="Freedea - User Manual" \
  --metadata subtitle="Open-source Wi-Fi controller for Midea air conditioners on the Xteink X4" \
  --metadata date="$DATE" \
  --metadata toc-title="Contents" \
  --metadata lang=en \
  --css="file://$CSS_FILE" \
  --pdf-engine=weasyprint \
  -o "$OUTPUT"

echo "wrote $OUTPUT ($(wc -c <"$OUTPUT") bytes)"
