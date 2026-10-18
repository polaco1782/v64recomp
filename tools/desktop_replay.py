#!/usr/bin/env python3
"""Run a native game on X11, replay keyboard actions and capture only its child window."""
from __future__ import annotations

import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

from common import RecompError


# The shared renderer's keyboard bindings. Arrow keys quantize analog input.
BINDINGS = {0x8000: 'a', 0x4000: 'x', 0x2000: 'z', 0x1000: 's',
            0x0800: 't', 0x0400: 'g', 0x0200: 'f', 0x0100: 'h',
            0x0020: 'c', 0x0010: 'v', 0x0008: 'i', 0x0004: 'k',
            0x0002: 'j', 0x0001: 'l'}
D, W = C.c_void_p, C.c_ulong


class KeyEvent(C.Structure):
    _fields_ = [('type', C.c_int), ('serial', W), ('send_event', C.c_int), ('display', D),
                ('window', W), ('root', W), ('subwindow', W), ('time', W),
                ('x', C.c_int), ('y', C.c_int), ('x_root', C.c_int), ('y_root', C.c_int),
                ('state', C.c_uint), ('keycode', C.c_uint), ('same_screen', C.c_int)]


class Event(C.Union):
    _fields_ = [('key', KeyEvent), ('pad', C.c_long * 24)]


class X11:
    def __init__(self):
        self.lib = C.CDLL('libX11.so.6')
        signatures = {
            'XOpenDisplay': ([C.c_char_p], D),
            'XDefaultRootWindow': ([D], W),
            'XInternAtom': ([D, C.c_char_p, C.c_int], W),
            'XQueryTree': ([D, W, C.POINTER(W), C.POINTER(W), C.POINTER(C.POINTER(W)), C.POINTER(C.c_uint)], C.c_int),
            'XGetWindowProperty': ([D, W, W, C.c_long, C.c_long, C.c_int, W, C.POINTER(W), C.POINTER(C.c_int), C.POINTER(W), C.POINTER(W), C.POINTER(C.c_void_p)], C.c_int),
            'XStringToKeysym': ([C.c_char_p], W),
            'XKeysymToKeycode': ([D, W], C.c_ubyte),
            'XRaiseWindow': ([D, W], C.c_int),
            'XSetInputFocus': ([D, W, C.c_int, W], C.c_int),
            'XSendEvent': ([D, W, C.c_int, C.c_long, C.POINTER(Event)], C.c_int),
            'XFlush': ([D], C.c_int),
            'XFree': ([C.c_void_p], C.c_int),
            'XCloseDisplay': ([D], C.c_int),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.lib, name)
            function.argtypes, function.restype = arguments, result
        self.display = self.lib.XOpenDisplay(None)
        if not self.display:
            raise RecompError('an accessible X11/Xwayland desktop is required')
        self.root = self.lib.XDefaultRootWindow(self.display)
        self.pid_atom = self.lib.XInternAtom(self.display, b'_NET_WM_PID', 0)

    def children(self, window: int) -> list[int]:
        root, parent, items, count = W(), W(), C.POINTER(W)(), C.c_uint()
        if not self.lib.XQueryTree(self.display, window, C.byref(root), C.byref(parent), C.byref(items), C.byref(count)):
            return []
        try:
            return [items[i] for i in range(count.value)]
        finally:
            if items:
                self.lib.XFree(items)

    def pid(self, window: int) -> int | None:
        kind, fmt, count, remaining, data = W(), C.c_int(), W(), W(), C.c_void_p()
        status = self.lib.XGetWindowProperty(self.display, window, self.pid_atom, 0, 1, 0, 0,
            C.byref(kind), C.byref(fmt), C.byref(count), C.byref(remaining), C.byref(data))
        try:
            if status == 0 and data.value and count.value == 1 and fmt.value == 32:
                return C.cast(data, C.POINTER(W))[0]
        finally:
            if data.value:
                self.lib.XFree(data)
        return None

    def find_window(self, pid: int) -> int | None:
        pending = [(self.root, 0)]
        while pending:
            window, depth = pending.pop()
            if self.pid(window) == pid:
                return window
            if depth < 4:
                pending.extend((child, depth+1) for child in self.children(window))
        return None

    def key(self, window: int, name: str, pressed: bool) -> None:
        self.lib.XRaiseWindow(self.display, window)
        self.lib.XSetInputFocus(self.display, window, 1, 0)
        code = self.lib.XKeysymToKeycode(self.display, self.lib.XStringToKeysym(name.encode()))
        if not code:
            raise RecompError(f'keyboard has no keycode for {name}')
        event = Event()
        event.key = KeyEvent(type=2 if pressed else 3, display=self.display, window=window,
            root=self.root, x=10, y=10, x_root=10, y_root=10, keycode=code, same_screen=1)
        # Address the child directly; global XTEST injection can miss Xwayland focus.
        if not self.lib.XSendEvent(self.display, window, 0, 1 if pressed else 2, C.byref(event)):
            raise RecompError('could not send an event to the child window')
        self.lib.XFlush(self.display)

    def close(self) -> None:
        self.lib.XCloseDisplay(self.display)


def replay_events(raw: str, frames: int, delay: float) -> list[tuple[float, str, bool]]:
    events, held, previous = [], set(), -1
    for line_number, line in enumerate(raw.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        try:
            frame, buttons, stick_x, stick_y = map(int, line.split())
        except ValueError as error:
            raise RecompError(f'replay line {line_number}: expected FRAME BUTTONS X Y in decimal') from error
        if (not previous <= frame < frames or frame < 0 or not 0 <= buttons <= 0xffff
                or buttons & ~sum(BINDINGS) or not -128 <= stick_x <= 127 or not -128 <= stick_y <= 127):
            raise RecompError(f'replay line {line_number}: invalid, unsorted or out-of-budget controller state')
        keys = {name for mask, name in BINDINGS.items() if buttons & mask}
        if stick_x:
            keys.add('Right' if stick_x > 0 else 'Left')
        if stick_y:
            keys.add('Up' if stick_y > 0 else 'Down')
        events.extend((frame/60+delay, name, False) for name in sorted(held-keys))
        events.extend((frame/60+delay, name, True) for name in sorted(keys-held))
        held, previous = keys, frame
    return events


def run(args) -> int:
    raw = args.input.read_text() if args.input else ''
    events = replay_events(raw, args.frames, args.start_delay)
    shots = sorted(args.capture_at)
    timeout = args.timeout if args.timeout is not None else args.frames/60+30
    if (not 0 < args.frames <= 0xffffffff or not 0 <= args.start_delay <= 30
            or not 0 < timeout or any(not 0 <= shot < timeout for shot in shots)
            or any(event[0] >= timeout for event in events)):
        raise RecompError('invalid frame budget, timing or capture schedule')
    if shots and not shutil.which('import'):
        raise RecompError('captures require ImageMagick import; omit --capture-at to run without captures')
    command = [str(args.executable.resolve()), '--frames', str(args.frames)]
    args.output.mkdir(parents=True, exist_ok=True)
    desktop = X11()
    process, held, captures, sent, timed_out = None, set(), [], 0, False
    start = time.monotonic()
    try:
        with (args.output/'run.log').open('w') as log:
            process = subprocess.Popen(command, stdout=log, stderr=log)
            window = None
            while process.poll() is None and time.monotonic()-start < min(15, timeout):
                window = desktop.find_window(process.pid)
                if window:
                    break
                time.sleep(.05)
            if window is None:
                raise RecompError('the launched process has no X11 window with its PID')
            while process.poll() is None and time.monotonic()-start < timeout:
                elapsed = time.monotonic()-start
                while events and elapsed >= events[0][0]:
                    _, name, pressed = events.pop(0)
                    desktop.key(window, name, pressed)
                    (held.add if pressed else held.discard)(name)
                    sent += 1
                while shots and elapsed >= shots[0]:
                    second = shots.pop(0)
                    path = args.output/f'capture-{second:07.2f}s.png'
                    try:
                        result = subprocess.run(['import', '-window', hex(window), str(path)],
                            capture_output=True, text=True, timeout=5)
                        error = result.stderr.strip() if result.returncode else None
                    except subprocess.TimeoutExpired:
                        error = 'window capture timed out'
                    captures.append(dict(seconds=second, path=str(path), error=error))
                time.sleep(.01)
            if process.poll() is None:
                timed_out = True
                for name in held:
                    desktop.key(window, name, False)
                process.terminate()
            try:
                exit_code = process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                exit_code = process.wait()
        complete = exit_code == 0 and not timed_out and not events and not shots and not any(c['error'] for c in captures)
        report = dict(format_version=1, command=command, input=str(args.input) if args.input else None,
            input_sha256=hashlib.sha256(raw.encode()).hexdigest(),
            input_clock='wall time at 60 Hz; nonzero stick axes mapped to arrow keys',
            elapsed_seconds=time.monotonic()-start, exit_code=exit_code, timed_out=timed_out,
            complete=complete, key_events_sent=sent, captures=captures)
        (args.output/'run.json').write_text(json.dumps(report, indent=2)+'\n')
        print(f'Exit {exit_code}; replay {"completed" if complete else "incomplete"}; report {args.output / "run.json"}')
        return 0 if complete else 1
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        desktop.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path, help='native app accepting --frames N')
    parser.add_argument('--frames', required=True, type=int, help='bounded app frame budget at 60 Hz')
    parser.add_argument('--input', type=Path, help='FRAME BUTTONS X Y text replay; omit for an idle run')
    parser.add_argument('--output', required=True, type=Path, help='directory for run.log, run.json and captures')
    parser.add_argument('--capture-at', action='append', type=float, default=[], metavar='SECONDS', help='capture the child window at this wall time; repeatable')
    parser.add_argument('--start-delay', type=float, default=.2, help='seconds added to replay key timestamps')
    parser.add_argument('--timeout', type=float, help='wall time limit; default frame duration plus 30 seconds')
    return run(parser.parse_args())


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RecompError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
