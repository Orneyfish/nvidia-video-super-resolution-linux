"""Material You colours from caelestia-shell's scheme.json and the CSS that maps them onto libadwaita.

Pure string handling (no GTK import) so it is unit tested. Pattern copied from ~/Projects/rog-control/main.py
(build_material_css), reduced to what this app uses; the .suggested-action / .destructive-action colours are set
explicitly because libadwaita's accent indirection ignores @define-color for them.
"""

import json

from . import paths

FALLBACK_MATERIAL_COLORS = {
    "surface": "#1a1112", "surfaceDim": "#1a1112", "surfaceBright": "#413735",
    "surfaceContainerLowest": "#140c0d", "surfaceContainerLow": "#231819", "surfaceContainer": "#271c1d",
    "surfaceContainerHigh": "#322627", "surfaceContainerHighest": "#3d3132",
    "onSurface": "#f8e0e0", "onSurfaceVariant": "#bba6a6",
    "primary": "#f7b6b9", "onPrimary": "#603337", "primaryContainer": "#7a4a4d", "onPrimaryContainer": "#ffdadb",
    "secondary": "#e6bdbe", "onSecondary": "#55393a", "secondaryContainer": "#6d4f50",
    "onSecondaryContainer": "#ffdadb",
    "tertiary": "#e6bdbe", "onTertiary": "#55393a", "tertiaryContainer": "#6d4f50", "onTertiaryContainer": "#ffdadb",
    "outline": "#847172", "outlineVariant": "#524344",
    "error": "#f97758", "onError": "#450900", "errorContainer": "#852c0a", "onErrorContainer": "#ffdad2",
    "success": "#8fd6a0", "onSuccess": "#00391a",
}


def load_scheme(path=None):
    """(colours dict with '#' prefixes, mode 'dark'|'light'). Falls back to the static palette."""
    path = path or paths.scheme_json()
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        # caelestia nests the palette under "colours"; accept a flat file as well
        colours = data.get("colours") if isinstance(data.get("colours"), dict) else data
        if colours.get("surface") and colours.get("primary"):
            merged = dict(FALLBACK_MATERIAL_COLORS)
            merged.update({k: (v if str(v).startswith("#") else f"#{v}")
                           for k, v in colours.items() if isinstance(v, str)})
            mode = data.get("mode") if data.get("mode") in ("dark", "light") else "dark"
            return merged, mode
    except (OSError, ValueError, AttributeError):
        pass
    return dict(FALLBACK_MATERIAL_COLORS), "dark"


def hex_to_rgba(hexval, alpha=1.0):
    h = hexval.lstrip("#")
    return int(h[0:2], 16) / 255, int(h[2:4], 16) / 255, int(h[4:6], 16) / 255, alpha


def build_material_css(c):
    def col(name):
        return c.get(name, FALLBACK_MATERIAL_COLORS.get(name, "#888888"))

    def rgba(name, a):
        r, g, b, _ = hex_to_rgba(col(name))
        return f"rgba({int(r * 255)}, {int(g * 255)}, {int(b * 255)}, {a})"

    return f"""
    @define-color accent_color {col('primary')};
    @define-color accent_bg_color {col('primary')};
    @define-color accent_fg_color {col('onPrimary')};
    @define-color destructive_color {col('error')};
    @define-color destructive_bg_color {col('error')};
    @define-color destructive_fg_color {col('onError')};
    @define-color success_color {col('success')};
    @define-color error_color {col('error')};
    @define-color window_bg_color {col('surface')};
    @define-color window_fg_color {col('onSurface')};
    @define-color view_bg_color {col('surface')};
    @define-color view_fg_color {col('onSurface')};
    @define-color headerbar_bg_color {col('surface')};
    @define-color headerbar_fg_color {col('onSurface')};
    @define-color headerbar_border_color {col('outlineVariant')};
    @define-color card_bg_color {col('surfaceContainer')};
    @define-color card_fg_color {col('onSurface')};
    @define-color popover_bg_color {col('surfaceContainerHigh')};
    @define-color popover_fg_color {col('onSurface')};
    @define-color dialog_bg_color {col('surfaceContainerHigh')};
    @define-color dialog_fg_color {col('onSurface')};

    window, window.background {{ background-color: {col('surface')}; color: {col('onSurface')}; }}
    headerbar {{
        background-color: {col('surface')}; color: {col('onSurface')};
        box-shadow: none; border-bottom: 1px solid {col('outlineVariant')};
    }}
    toolbarview > .bottom-bar, toolbarview > .bottom-bar viewswitcherbar {{ background-color: {col('surfaceContainer')}; }}

    viewswitcher button:checked, viewswitcherbar button:checked, viewswitcherbutton:checked {{
        background-color: {col('secondaryContainer')}; color: {col('onSecondaryContainer')};
    }}

    button {{ border-radius: 20px; }}
    button.suggested-action {{ background-color: {col('primary')}; color: {col('onPrimary')}; }}
    button.suggested-action:hover {{ background-color: {rgba('primary', 0.9)}; }}
    button.suggested-action:disabled {{ background-color: {rgba('onSurface', 0.12)}; color: {rgba('onSurface', 0.38)}; }}
    button.destructive-action {{ background-color: {col('error')}; color: {col('onError')}; }}

    list.boxed-list, .boxed-list {{ background-color: {col('surfaceContainer')}; border-radius: 16px;
        border: 1px solid {col('outlineVariant')}; }}
    list.boxed-list > row {{ background-color: transparent; }}
    row label.subtitle {{ color: {col('onSurfaceVariant')}; }}
    preferencesgroup > box > box > label.heading {{ color: {col('onSurfaceVariant')}; }}

    .card, box.card {{ background-color: {col('surfaceContainer')}; color: {col('onSurface')};
        border-radius: 16px; border: 1px solid {col('outlineVariant')}; box-shadow: none; }}
    .nvc-hero {{ background-color: {col('surfaceContainerHigh')}; border-radius: 20px; padding: 16px; }}
    .nvc-tile {{ background-color: {col('surfaceContainer')}; border: 1px solid {col('outlineVariant')};
        border-radius: 16px; padding: 14px 16px; }}
    .nvc-tile-caption {{ color: {col('onSurfaceVariant')}; font-size: 82%; font-weight: 600; letter-spacing: 0.4px; }}
    .nvc-tile-value {{ font-size: 140%; font-weight: 700; }}
    .nvc-tile-sub {{ color: {col('onSurfaceVariant')}; font-size: 85%; }}
    .nvc-stale {{ opacity: 0.55; }}
    .nvc-muted {{ color: {col('onSurfaceVariant')}; }}
    .nvc-hint {{ color: {col('onSurfaceVariant')}; font-size: 88%; }}
    .nvc-error {{ color: {col('error')}; }}
    .nvc-cmd {{ font-family: monospace; background-color: {col('surfaceContainerHighest')}; color: {col('onSurface')};
        border-radius: 8px; padding: 4px 8px; }}
    .nvc-chip {{ border-radius: 999px; padding: 2px 12px; font-size: 85%; font-weight: 600;
        background-color: {col('surfaceContainerHighest')}; color: {col('onSurfaceVariant')}; }}
    .nvc-chip-ok {{ background-color: {rgba('success', 0.22)}; color: {col('success')}; }}
    .nvc-chip-warn {{ background-color: {rgba('tertiary', 0.22)}; color: {col('tertiary')}; }}
    .nvc-chip-bad {{ background-color: {rgba('error', 0.22)}; color: {col('error')}; }}
    .nvc-ok {{ color: {col('success')}; }}
    .nvc-warn {{ color: {col('tertiary')}; }}
    .nvc-bad {{ color: {col('error')}; }}
    .nvc-banner {{ background-color: {col('errorContainer')}; color: {col('onErrorContainer')};
        border-radius: 12px; padding: 8px 12px; }}

    progressbar trough {{ background-color: {col('surfaceContainerHighest')}; border-radius: 999px; min-height: 8px; }}
    progressbar progress {{ background-color: {col('primary')}; border-radius: 999px; min-height: 8px; }}
    progressbar.nvc-warn progress {{ background-color: {col('tertiary')}; }}
    progressbar.nvc-bad progress {{ background-color: {col('error')}; }}

    switch {{ background-color: {col('surfaceContainerHighest')}; border: 1px solid {col('outline')}; border-radius: 999px; }}
    switch:checked {{ background-color: {col('primary')}; border-color: {col('primary')}; }}
    switch slider {{ background-color: {col('outline')}; border-radius: 999px; }}
    switch:checked slider {{ background-color: {col('onPrimary')}; }}
    scale trough {{ background-color: {col('surfaceContainerHighest')}; border-radius: 999px; min-height: 8px; }}
    scale highlight {{ background-color: {col('primary')}; border-radius: 999px; }}
    scale slider {{ background-color: {col('primary')}; border-radius: 999px; }}
    spinbutton, entry {{ border-radius: 12px; }}
    dropdown > button {{ border-radius: 12px; }}
    toast {{ background-color: {col('surfaceContainerHighest')}; color: {col('onSurface')}; border-radius: 16px; }}
    separator {{ background-color: {col('outlineVariant')}; }}
    """
