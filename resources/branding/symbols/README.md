# Dinero Symbol Pack

## Files
- `dinero-symbol.svg`: primary Dinero currency symbol
- `dinero-symbol-small.svg`: small-size version for 16-24px UI
- `dinero-symbol.css`: immediate web usage (SVG mask) + icon-font wiring
- `dinero-symbol-map.json`: codepoint mapping for icon-font generation

## Quick Web Use (no font needed)
```html
<link rel="stylesheet" href="dinero-symbol.css">
<span class="din-symbol-icon" aria-label="Dinero"></span>
```

## Typed Icon-Font Mode
Use the mapping from `dinero-symbol-map.json`:
- `U+E900` -> primary symbol
- `U+E901` -> small symbol

After generating `DineroSymbol.woff2` (or `.ttf`) and placing it next to the CSS:
```html
<span class="din-symbol-font" aria-label="Dinero"></span>
```

## iOS/App fallback guidance
- Keep `DIN` as fallback ticker text.
- Use SVG-rendered symbol in design surfaces and iconography.
- If you add a custom app font later, map `U+E900/U+E901` to these glyphs.
