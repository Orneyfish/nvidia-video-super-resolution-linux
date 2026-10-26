"""Shared widgets and helpers for NV Control's pages (GTK4 + libadwaita)."""

import os
import threading

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, GLib, Gtk  # noqa: E402

import cairo

from nvc import conf, paths, proc


# ------------------------------------------------------------------------------------------------------------

def run_async(func, done):
    """Run func() in a thread, call done(result) on the main loop. An exception becomes the result."""
    def worker():
        try:
            res = func()
        except Exception as e:  # noqa: BLE001 - never let a worker traceback reach the UI
            res = e
        GLib.idle_add(_deliver, done, res)

    threading.Thread(target=worker, daemon=True).start()


def _deliver(done, res):
    done(res)
    return GLib.SOURCE_REMOVE


def short_path(p):
    h = paths.home()
    return "~" + p[len(h):] if h and p.startswith(h + os.sep) else p


def fmt_num(v, digits=0, unit=""):
    if v is None:
        return "-"
    return f"{v:.{digits}f}{unit}"


def fmt_mib(v):
    if v is None:
        return "-"
    return f"{v / 1024:.1f} GiB" if v >= 1024 else f"{v:.0f} MiB"


def chip(text, kind=None):
    classes = ["nvc-chip"] + ([f"nvc-chip-{kind}"] if kind else [])
    return Gtk.Label(label=text, css_classes=classes, valign=Gtk.Align.CENTER)


def hint(text, error=False):
    lbl = Gtk.Label(label=text, xalign=0, wrap=True, css_classes=["nvc-error" if error else "nvc-hint"])
    return lbl


def make_page(child):
    clamp = Adw.Clamp(maximum_size=860, tightening_threshold=640, margin_top=18, margin_bottom=24,
                      margin_start=16, margin_end=16, child=child)
    return Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER, vexpand=True, child=clamp)


def vbox(spacing=18):
    return Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=spacing)


def kv_row(title, subtitle=None):
    row = Adw.ActionRow(title=title)
    if subtitle:
        row.set_subtitle(subtitle)
    value = Gtk.Label(label="-", xalign=1, selectable=True, css_classes=["nvc-muted"],
                      valign=Gtk.Align.CENTER)
    row.add_suffix(value)
    return row, value


def icon_image(name, css=None):
    img = Gtk.Image.new_from_icon_name(name)
    if css:
        img.add_css_class(css)
    return img


class Tile(Gtk.Box):
    def __init__(self, caption, bar=True):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=4, css_classes=["nvc-tile"])
        self.value = Gtk.Label(label="-", xalign=0, css_classes=["nvc-tile-value"])
        self.sub = Gtk.Label(label="", xalign=0, wrap=True, css_classes=["nvc-tile-sub"])
        self.bar = Gtk.ProgressBar() if bar else None
        self.append(Gtk.Label(label=caption.upper(), xalign=0, css_classes=["nvc-tile-caption"]))
        self.append(self.value)
        if self.bar:
            self.append(self.bar)
        self.append(self.sub)

    def set(self, value, sub="", fraction=None, level=None):
        self.value.set_label(value)
        self.sub.set_label(sub)
        if self.bar:
            self.bar.set_fraction(0.0 if fraction is None else max(0.0, min(1.0, fraction)))
            for c in ("nvc-warn", "nvc-bad"):
                self.bar.remove_css_class(c)
            if level:
                self.bar.add_css_class(level)


# ------------------------------------------------------------------------------------------------------------

class BadgePreview(Gtk.DrawingArea):
    """Miniature of a 1280x720 frame with the status badge where and how mpv would draw it (shown 2x larger)."""

    def __init__(self):
        super().__init__(hexpand=True, margin_top=6, margin_bottom=6)
        self.state = dict(shown=True, size=11, opacity=0.55, position=9)
        self.set_draw_func(self._draw)

    def do_measure(self, orientation, for_size):  # 16:9, height follows the width
        if orientation == Gtk.Orientation.HORIZONTAL:
            return 120, 480, -1, -1
        h = int((for_size if for_size > 0 else 480) * 9 / 16)
        return h, h, -1, -1

    def update(self, shown, size, opacity, position):
        self.state = dict(shown=shown, size=size, opacity=opacity, position=position)
        self.queue_draw()

    def _draw(self, area, cr, w, h):
        st = self.state
        grad = cairo.LinearGradient(0, 0, w, h)
        grad.add_color_stop_rgb(0, 0.10, 0.14, 0.20)
        grad.add_color_stop_rgb(1, 0.30, 0.34, 0.38)
        cr.rectangle(0, 0, w, h)
        cr.set_source(grad)
        cr.fill()
        scale = w / 1280.0
        text = "SR" if st["shown"] else ""
        if not text:
            cr.set_source_rgba(1, 1, 1, 0.5)
            cr.set_font_size(12)
            cr.move_to(10, h / 2)
            cr.show_text("Badge off")
            return
        cr.select_font_face("sans-serif", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
        cr.set_font_size(st["size"] * scale * 2)
        ext = cr.text_extents(text)
        margin = 14 * scale * 2
        col, row = (st["position"] - 1) % 3, (st["position"] - 1) // 3  # col 0 left..2 right, row 0 bottom..2 top
        x = margin if col == 0 else (w - ext.width) / 2 - ext.x_bearing if col == 1 else w - margin - ext.width - ext.x_bearing
        y = h - margin if row == 0 else (h + ext.height) / 2 if row == 1 else margin + ext.height
        cr.move_to(x, y)
        cr.text_path(text)
        cr.set_source_rgba(0, 0, 0, st["opacity"])
        cr.set_line_width(max(1.0, scale * 2))
        cr.stroke_preserve()
        cr.set_source_rgba(1, 1, 1, st["opacity"])
        cr.fill()


class ConfEditor:
    """An Adw.PreferencesGroup editing a native config file through nvc.conf (comment-preserving, atomic)."""

    def __init__(self, win, title, options, path_fn, *, section=None, bool_style="yesno", guard=None,
                 on_change=None, noun="settings"):
        self.win, self.options, self.path_fn = win, options, path_fn
        self.section, self.bool_style, self.guard, self.on_change, self.noun = section, bool_style, guard, on_change, noun
        self.by_key = {o.key: o for o in options}
        self.ctl, self.loaded, self._loading, self._busy = {}, None, False, False
        self.group = Adw.PreferencesGroup(title=title)
        self.banner = Gtk.Label(xalign=0, wrap=True, css_classes=["nvc-banner"], visible=False)
        basics = [o for o in options if not o.advanced]
        adv = [o for o in options if o.advanced]
        for o in basics:
            self.group.add(self._make_row(o))
        if adv:
            exp = Adw.ExpanderRow(title="Advanced")
            for o in adv:
                exp.add_row(self._make_row(o))
            self.group.add(exp)
        self.msg = hint("")
        self.revert_btn = Gtk.Button(label="Revert")
        self.save_btn = Gtk.Button(label="Save", css_classes=["suggested-action"])
        self.revert_btn.connect("clicked", lambda *_: self.reload())
        self.save_btn.connect("clicked", lambda *_: self.save())
        foot = Gtk.Box(spacing=8, margin_top=6)
        self.msg.set_hexpand(True)
        foot.append(self.msg)
        foot.append(self.revert_btn)
        foot.append(self.save_btn)
        # a non-row widget added to a PreferencesGroup lands below its list, so the banner and footer are siblings
        self.widget = vbox(8)
        for w in (self.banner, self.group, foot):
            self.widget.append(w)
        self.reload()

    def insert_extra(self, w):
        """Put a widget between the option list and the Save/Revert row."""
        self.widget.insert_child_after(w, self.group)

    # -- rows -----------------------------------------------------------------------------------------------
    def _make_row(self, o):
        if o.kind == "bool":
            row = Adw.SwitchRow(title=o.label)
            row.connect("notify::active", self._changed)
            self.ctl[o.key] = (row, row.get_active, row.set_active)
        elif o.kind in ("enum", "enumint", "choice"):
            model = Gtk.StringList.new([lab for _, lab in o.choices])
            values = [v for v, _ in o.choices]
            row = Adw.ComboRow(title=o.label, model=model)
            row.connect("notify::selected", self._changed)

            def get(row=row, values=values):
                return values[row.get_selected()] if row.get_selected() < len(values) else None

            def set_(v, row=row, model=model, values=values):
                if v not in values:  # a value found in the file that is not one of the presets: keep it selectable
                    values.append(v)
                    model.append(str(v))
                row.set_selected(values.index(v))
            self.ctl[o.key] = (row, get, set_)
        elif o.kind in ("int", "float"):
            step = 1 if o.kind == "int" else (0.05 if o.hi <= 1 else 0.5)
            row = Adw.SpinRow(title=o.label, digits=0 if o.kind == "int" else max(o.digits, 1),
                              adjustment=Gtk.Adjustment(value=o.default, lower=o.lo, upper=o.hi, step_increment=step,
                                                        page_increment=step * 10))
            row.connect("notify::value", self._changed)
            self.ctl[o.key] = (row, lambda row=row: row.get_value(), lambda v, row=row: row.set_value(float(v)))
        else:  # float? / str: free text, validated on save
            title = o.label + (" (0 to 1, empty = SDK default)" if o.kind == "float?" else "")
            row = Adw.EntryRow(title=title)
            row.connect("changed", self._changed)
            self.ctl[o.key] = (row, row.get_text, lambda v, row=row: row.set_text(str(v)))
        row.set_tooltip_text(o.help)
        try:
            row.update_property([Gtk.AccessibleProperty.DESCRIPTION], [o.help])
        except (TypeError, AttributeError):
            pass
        return row

    def value(self, key):
        """Typed value of one control (the raw text if it does not parse yet)."""
        o = self.by_key[key]
        raw = self.ctl[key][1]()
        if o.kind == "int" or o.kind == "enumint":
            return int(round(raw)) if isinstance(raw, float) else raw
        if o.kind == "float":
            return round(float(raw), 4)
        if o.kind == "float?":
            try:
                return conf.parse_value(o, raw)
            except ValueError:
                return raw
        return raw

    def current_values(self):
        return {k: self.value(k) for k in self.ctl}

    def set_value(self, key, v):
        self._loading = True
        try:
            self.ctl[key][2](v)
        finally:
            self._loading = False
        self._changed()

    # -- state ----------------------------------------------------------------------------------------------
    def reload(self):
        path = self.path_fn()
        self.loaded = conf.load_settings(path, self.options)
        self._loading = True
        try:
            for k, v in self.loaded.values.items():
                self.ctl[k][2](v)
        finally:
            self._loading = False
        note = short_path(path) + ("" if self.loaded.exists else " (not created yet, saving creates it)")
        self.group.set_description(GLib.markup_escape_text(note))
        self.set_message("; ".join(self.loaded.problems[:2]) + (" - shown as default" if self.loaded.problems else ""),
                         error=bool(self.loaded.problems))
        self._changed()

    def set_message(self, text, error=False):
        self.msg.set_label(text or "")
        (self.msg.add_css_class if error else self.msg.remove_css_class)("nvc-error")
        (self.msg.remove_css_class if error else self.msg.add_css_class)("nvc-hint")

    def set_banner(self, text):
        self.banner.set_label(text or "")
        self.banner.set_visible(bool(text))

    def is_dirty(self):
        cur = self.current_values()
        return any(cur[k] != self.loaded.values.get(k) for k in cur)

    def _changed(self, *_):
        if self._loading or self.loaded is None:
            return
        dirty = self.is_dirty()
        self.save_btn.set_sensitive(dirty and not self._busy)
        self.revert_btn.set_sensitive(dirty)
        if self.on_change:
            self.on_change(self.current_values())

    # -- saving ---------------------------------------------------------------------------------------------
    def validate(self, values):
        errors = []
        for k, v in values.items():
            o = self.by_key[k]
            if o.kind == "float?" and isinstance(v, str) and v != "":
                errors.append(f"{o.label}: enter a number between {o.lo:g} and {o.hi:g}, or leave it empty")
            elif o.kind == "str" and ("\n" in str(v) or "\r" in str(v)):
                errors.append(f"{o.label}: a single line is expected")
        return errors

    def save(self, force=False, done=None):
        if self._busy:
            return
        values = self.current_values()
        errors = self.validate(values)
        if errors:
            self.set_message(errors[0], error=True)
            return
        self._busy = True
        self.save_btn.set_sensitive(False)

        def after_guard(blocked):
            if isinstance(blocked, Exception):
                blocked = None
            if blocked:
                self._busy = False
                self.set_message(blocked, error=True)
                self._changed()
                return
            self._write(values, force, done)

        if self.guard:
            run_async(self.guard, after_guard)
        else:
            after_guard(None)

    def _write(self, values, force, done):
        res = conf.save_changes(self.path_fn(), self.options, values, self.loaded, section=self.section,
                                bool_style=self.bool_style, force=force)
        self._busy = False
        if res.conflicts:
            self.set_message("The file changed on disk since it was loaded.", error=True)
            self._ask_conflict(res.conflicts, values)
        elif not res.ok:
            self.set_message(res.error or "Could not save.", error=True)
            self._changed()
        else:
            self.reload()
            self.set_message("Saved." + (f" Original kept as {short_path(res.backup)}." if res.backup else ""))
            self.win.toast(f"Saved {self.noun}")
        if done:
            done(res)

    def _ask_conflict(self, conflicts, values):
        d = Adw.AlertDialog(
            heading="File changed outside NV Control",
            body=f"{os.path.basename(self.path_fn())} was edited since it was loaded ({', '.join(conflicts)}). "
                 "Reload to see the current values, or overwrite just those keys with yours.")
        d.add_response("reload", "Reload")
        d.add_response("overwrite", "Overwrite those keys")
        d.set_response_appearance("overwrite", Adw.ResponseAppearance.DESTRUCTIVE)
        d.set_default_response("reload")
        d.set_close_response("reload")

        def answered(dialog, result):
            try:
                choice = dialog.choose_finish(result)
            except GLib.Error:
                choice = "reload"
            if choice == "overwrite":
                self.save(force=True)
            else:
                self.reload()

        d.choose(self.win, None, answered)


def vlc_running():
    """Message when VLC is running (it rewrites vlcrc on exit and would discard our edit), else None."""
    r = proc.run(["pgrep", "-x", "vlc"], timeout=3)
    if r.rc == 0:
        return "VLC is running. It rewrites vlcrc when it exits and would discard these changes. Close VLC first."
    return None


