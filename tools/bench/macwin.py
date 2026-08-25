#!/usr/bin/env python3
"""Find, park and photograph the emulator window without touching the display.

Three constraints shaped this. macOS Accessibility is not granted here, so
osascript and synthetic keystrokes are out -- they hang on a TCC prompt that
nothing can answer. A full-screen `screencapture` is never acceptable: it
photographs whatever else the user has open, which has already happened once
and must not happen again. And the machine has to stay usable while a
measurement runs, so the emulator cannot own the display or steal the keyboard.

What is left is enough. CGWindowListCopyWindowInfo needs no permission beyond
Screen Recording and gives a window id; `screencapture -l <id>` photographs
exactly that window and nothing behind it. Position comes from the wxConfig
file window_wx.cc already reads at frame construction, so the window can be
parked before it is ever shown rather than moved afterwards -- moving it
afterwards is the part that needs Accessibility.

Driving is separate: --input_script feeds the guest through a normal
InputDriver, so the window never needs focus to be controlled.
"""
import argparse, os, subprocess, sys

import Quartz

# wxWidgets writes this from SetAppName()/SetVendorName() in windowed_app_wx.cc.
WX_CONFIG = os.path.expanduser("~/Library/Preferences/xenia_edge Preferences")


def windows(pid=None, owner_substr=None):
    """On-screen windows, optionally filtered by owning pid or app name."""
    info = Quartz.CGWindowListCopyWindowInfo(
        Quartz.kCGWindowListOptionOnScreenOnly
        | Quartz.kCGWindowListExcludeDesktopElements,
        Quartz.kCGNullWindowID) or []
    out = []
    for w in info:
        owner = w.get("kCGWindowOwnerName", "") or ""
        wpid = w.get("kCGWindowOwnerPID")
        if pid is not None and wpid != pid:
            continue
        if owner_substr and owner_substr.lower() not in owner.lower():
            continue
        b = w.get("kCGWindowBounds", {})
        out.append({
            "id": int(w.get("kCGWindowNumber", 0)),
            "pid": int(wpid or 0),
            "owner": owner,
            "name": w.get("kCGWindowName", "") or "",
            "layer": int(w.get("kCGWindowLayer", 0)),
            "x": int(b.get("X", 0)), "y": int(b.get("Y", 0)),
            "w": int(b.get("Width", 0)), "h": int(b.get("Height", 0)),
        })
    # The render window is the biggest normal-layer one; helper windows and
    # the menu bar sit on other layers or are tiny.
    out.sort(key=lambda d: (d["layer"] != 0, -(d["w"] * d["h"])))
    return out


def frontmost():
    """Name of the app that currently owns the keyboard, for focus checks."""
    for w in windows():
        if w["layer"] == 0:
            return w["owner"]
    return ""


def shot(window_id, path):
    """Photograph one window by id. Never the screen, never a region."""
    r = subprocess.run(["screencapture", "-x", "-o", "-l", str(window_id), path],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(path):
        return False, (r.stderr or "").strip()
    return True, ""


def park(x, y):
    """Seed the position window_wx.cc reads at frame construction.

    Both binaries under test share this file, so an A/B parks both refs
    identically and the geometry cannot become a difference between them.
    """
    lines, seen_window, wrote = [], False, {"x": False, "y": False}
    if os.path.exists(WX_CONFIG):
        for ln in open(WX_CONFIG).read().splitlines():
            s = ln.strip()
            if s.startswith("["):
                if seen_window:
                    for k, v in (("x", x), ("y", y)):
                        if not wrote[k]:
                            lines.append(f"{k}={v}")
                            wrote[k] = True
                seen_window = (s == "[window]")
            elif seen_window and s.startswith("x="):
                ln, wrote["x"] = f"x={x}", True
            elif seen_window and s.startswith("y="):
                ln, wrote["y"] = f"y={y}", True
            lines.append(ln)
    else:
        lines = ["[window]", "maximized=0"]
        seen_window = True
    if seen_window:
        for k, v in (("x", x), ("y", y)):
            if not wrote[k]:
                lines.append(f"{k}={v}")
    elif "[window]" not in "\n".join(lines):
        lines += ["[window]", "maximized=0", f"x={x}", f"y={y}"]
    open(WX_CONFIG, "w").write("\n".join(lines) + "\n")
    return x, y


def displays():
    """Bounds of every active display, main first.

    CGGetActiveDisplayList's out-parameter form comes back empty under this
    pyobjc, so the id list is built from CGMainDisplayID plus whatever the
    online list reports rather than trusting one call.
    """
    ids, main = [], Quartz.CGMainDisplayID()
    ids.append(main)
    try:
        err, online, n = Quartz.CGGetOnlineDisplayList(16, None, None)
        if not err:
            ids += [d for d in (online or ()) if d != main]
    except Exception:
        pass
    out = []
    for d in ids:
        b = Quartz.CGDisplayBounds(d)
        out.append((int(b.origin.x), int(b.origin.y),
                    int(b.size.width), int(b.size.height)))
    return out


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list"); p.add_argument("--pid", type=int)
    p.add_argument("--owner", default="")
    p = sub.add_parser("shot"); p.add_argument("out")
    p.add_argument("--pid", type=int); p.add_argument("--owner", default="Xenia")
    p = sub.add_parser("park"); p.add_argument("x", type=int); p.add_argument("y", type=int)
    sub.add_parser("displays")
    sub.add_parser("frontmost")
    a = ap.parse_args()
    if a.cmd == "list":
        for w in windows(a.pid, a.owner or None):
            print(f"{w['id']:>8}  pid={w['pid']:<7} layer={w['layer']:<3} "
                  f"{w['x']},{w['y']} {w['w']}x{w['h']}  {w['owner']!r} {w['name']!r}")
    elif a.cmd == "shot":
        ws = windows(a.pid, a.owner or None)
        if not ws:
            print("no matching window", file=sys.stderr); return 1
        ok, err = shot(ws[0]["id"], a.out)
        print(f"{'captured' if ok else 'FAILED'} window {ws[0]['id']} "
              f"({ws[0]['w']}x{ws[0]['h']}) -> {a.out} {err}")
        return 0 if ok else 1
    elif a.cmd == "park":
        print("parked at", park(a.x, a.y), "in", WX_CONFIG)
    elif a.cmd == "displays":
        for x, y, w, h in displays():
            print(f"{x},{y} {w}x{h}")
    elif a.cmd == "frontmost":
        print(frontmost())
    return 0


if __name__ == "__main__":
    sys.exit(main())
