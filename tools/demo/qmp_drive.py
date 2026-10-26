#!/usr/bin/env python3
"""Drive a running QEMU over QMP from a demo script (SCRUM-121).

Usage: qmp_drive.py <qmp-port> <script> <out-dir> <serial-log>

Script lines (blank lines and '#' comments ignored):

    wait <seconds>        sleep
    type <text>           type the text, then Enter
    key <a-b-...>         one key chord, QEMU qcodes joined by '-' (ctrl-tab, q)
    shot <name>           screendump to <out-dir>/<name>.png
    expect <regex>        after the run, the serial log must match this
    expect-not <regex>    ... and must never match this

Standard library only. Exits 0 when every expectation holds, 1 otherwise.
"""
import json
import os
import re
import socket
import sys
import time

KEYS = {
    " ": ["spc"], "/": ["slash"], ".": ["dot"], "-": ["minus"],
    "*": ["shift", "8"], "_": ["shift", "minus"], ":": ["shift", "semicolon"],
}


def connect(port):
    for _ in range(600):
        try:
            sock = socket.create_connection(("127.0.0.1", port))
            break
        except OSError:
            time.sleep(0.1)
    else:
        sys.exit("qmp_drive: QEMU's QMP socket never came up")
    f = sock.makefile("rw")
    f.readline()                              # greeting
    cmd(f, {"execute": "qmp_capabilities"})
    return f


def cmd(f, obj):
    f.write(json.dumps(obj) + "\n")
    f.flush()
    while True:
        reply = json.loads(f.readline())
        if "return" in reply or "error" in reply:
            return reply


def send_keys(f, keys):
    cmd(f, {"execute": "send-key", "arguments": {
        "keys": [{"type": "qcode", "data": k} for k in keys],
        "hold-time": 40}})
    time.sleep(0.05)


def type_text(f, text):
    for ch in text:
        if ch in KEYS:
            send_keys(f, KEYS[ch])
        elif ch.isupper():
            send_keys(f, ["shift", ch.lower()])
        else:
            send_keys(f, [ch])
    send_keys(f, ["ret"])
    time.sleep(0.6)


def main():
    port, script, out_dir, serial_log = (int(sys.argv[1]), sys.argv[2],
                                         sys.argv[3], sys.argv[4])
    os.makedirs(out_dir, exist_ok=True)
    expects, expect_nots = [], []
    f = connect(port)
    t0 = time.time()

    for raw in open(script):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        op, _, arg = line.partition(" ")
        if op == "wait":
            time.sleep(float(arg))
        elif op == "type":
            type_text(f, arg)
        elif op == "key":
            send_keys(f, arg.split("-"))
            time.sleep(0.8)
        elif op == "shot":
            path = os.path.abspath(os.path.join(out_dir, arg + ".png"))
            cmd(f, {"execute": "screendump",
                    "arguments": {"filename": path, "format": "png"}})
            print(f"  {time.time() - t0:6.1f}s  shot {arg}")
        elif op == "expect":
            expects.append(arg)
        elif op == "expect-not":
            expect_nots.append(arg)
        else:
            sys.exit(f"qmp_drive: unknown op {op!r}")

    try:
        cmd(f, {"execute": "quit"})
    except (OSError, ValueError):
        pass                      # QEMU may close the socket before replying
    time.sleep(1)

    log = open(serial_log, errors="replace").read()
    ok = True
    for pat in expects:
        hit = re.search(pat, log, re.MULTILINE) is not None
        ok &= hit
        print(f"  {'PASS' if hit else 'FAIL'}  expect     {pat}")
    for pat in expect_nots:
        hit = re.search(pat, log, re.MULTILINE) is not None
        ok &= not hit
        print(f"  {'FAIL' if hit else 'PASS'}  expect-not {pat}")
    print(f"rehearsal {'PASSED' if ok else 'FAILED'} in {time.time() - t0:.0f}s")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
