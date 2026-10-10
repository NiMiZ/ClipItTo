#!/usr/bin/env python3
"""Drive the real ClipItTo daemon on a private, headless X11 display.

Each subcommand is one step; state (display, D-Bus bus, daemon pid, temp config/history)
lives in a session directory, so steps can be run one at a time from a shell:

    driver.py start                 # Xvfb + private D-Bus bus + clipitto-daemon (Qt xcb)
    driver.py copy "some text"      # another app copies text (xclip) -> daemon captures it
    driver.py cli --toggle          # run the real `clipitto` CLI against this daemon
    driver.py type "query"          # type into the focused window (xdotool)
    driver.py key Down Return       # press keys (xdotool names: Down, Return, ctrl+g, Escape ...)
    driver.py ss popup              # screenshot -> <session>/screenshots/popup.png
    driver.py clipboard             # print what is on the clipboard now
    driver.py status                # `clipitto --status` as JSON
    driver.py stop                  # quit daemon, stop D-Bus and Xvfb
    driver.py smoke                 # full scripted user flow with checks (starts/stops itself)

Environment:
    CLIPITTO_BUILD        build dir with src/daemon/clipitto-daemon (default: build/debug)
    CLIPITTO_DRIVER_DIR   session dir (default: /tmp/clipitto-driver)
Tools needed on PATH: Xvfb, xdotool, xclip, dbus-daemon; Python Pillow (screenshots).
"""

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SESSION = Path(os.environ.get("CLIPITTO_DRIVER_DIR", Path(tempfile.gettempdir()) / "clipitto-driver"))
STATE = SESSION / "state.json"
BUILD = Path(os.environ.get("CLIPITTO_BUILD", "build/debug")).resolve()
DAEMON = BUILD / "src" / "daemon" / "clipitto-daemon"
CLI = BUILD / "src" / "cli" / "clipitto"

# Minimal session bus with no service directories: an installed ClipItTo can never be
# D-Bus-activated instead of the build under test.
BUS_CONF = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir={tmp}</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/>
  </policy>
</busconfig>
"""


def die(message):
    print(f"driver: {message}", file=sys.stderr)
    sys.exit(1)


def load_state():
    if not STATE.exists():
        die("not started (run: driver.py start)")
    return json.loads(STATE.read_text())


def app_env(state):
    """Environment of an X11 desktop session for the daemon, the CLI and the X tools."""
    env = dict(os.environ)
    for var in ("WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_ACTIVATION_TOKEN"):
        env.pop(var, None)  # never leak the host's (WSLg/GNOME) session in
    env.update(
        DISPLAY=state["display"],
        QT_QPA_PLATFORM="xcb",
        XDG_SESSION_TYPE="x11",
        XDG_CONFIG_HOME=str(SESSION / "config"),
        XDG_DATA_HOME=str(SESSION / "data"),
        DBUS_SESSION_BUS_ADDRESS=state["bus"],
    )
    return env


def run(state, *cmd, check=True, capture=True):
    result = subprocess.run([str(c) for c in cmd], env=app_env(state), text=True, capture_output=capture)
    if check and result.returncode != 0:
        die(f"{' '.join(map(str, cmd))} failed ({result.returncode}): {result.stderr.strip()}")
    return result


def display_answers(display):
    # Connect instead of looking for /tmp/.X11-unix/X<n>: where that directory is not
    # writable (WSLg mounts it), Xvfb listens on the abstract socket only and no file appears.
    env = dict(os.environ, DISPLAY=display)
    return subprocess.run(["xdotool", "getdisplaygeometry"], env=env, capture_output=True).returncode == 0


def free_display():
    for n in range(99, 140):
        if not Path(f"/tmp/.X{n}-lock").exists() and not display_answers(f":{n}"):
            return n
    die("no free X display number in :99-:139")


def wait_for(predicate, timeout, what):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    die(f"timed out waiting for {what}")


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


# --- commands -------------------------------------------------------------------------


def cmd_start(_args):
    if STATE.exists():
        state = json.loads(STATE.read_text())
        if state["daemon_pid"] and pid_alive(state["daemon_pid"]):
            die(f"already running on {state['display']} (driver.py stop first)")
        cmd_stop([])  # stale session: clean up first
    for tool in ("Xvfb", "xdotool", "xclip", "dbus-daemon"):
        if not shutil.which(tool):
            die(f"{tool} not found on PATH (see the skill's Prerequisites)")
    if not DAEMON.exists() or not CLI.exists():
        die(f"no build at {BUILD} (set CLIPITTO_BUILD or build first)")

    if SESSION.exists():
        shutil.rmtree(SESSION)
    for sub in ("config", "data", "screenshots", "logs"):
        (SESSION / sub).mkdir(parents=True)

    n = free_display()
    xvfb = subprocess.Popen(
        ["Xvfb", f":{n}", "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
        stdout=open(SESSION / "logs" / "xvfb.log", "w"), stderr=subprocess.STDOUT, start_new_session=True)
    state = {"display": f":{n}", "xvfb_pid": xvfb.pid, "bus": "", "bus_pid": 0, "daemon_pid": 0}
    STATE.write_text(json.dumps(state, indent=2))  # from here on, `stop` can clean up
    try:
        launch(state)
    except BaseException:
        cmd_stop([])
        raise
    print(f"started: display {state['display']}, daemon pid {state['daemon_pid']}, session {SESSION}")


def launch(state):
    wait_for(lambda: display_answers(state["display"]), 10, "Xvfb")

    (SESSION / "bus.conf").write_text(BUS_CONF.format(tmp=tempfile.gettempdir()))
    bus = subprocess.run(["dbus-daemon", f"--config-file={SESSION / 'bus.conf'}", "--fork",
                          "--print-address=1", "--print-pid=1"], capture_output=True, text=True, check=True)
    address, bus_pid = bus.stdout.split()[:2]
    state.update(bus=address, bus_pid=int(bus_pid))
    STATE.write_text(json.dumps(state, indent=2))
    daemon = subprocess.Popen([str(DAEMON)], env=app_env(state),
                              stdout=open(SESSION / "logs" / "daemon.log", "w"), stderr=subprocess.STDOUT,
                              start_new_session=True)
    state["daemon_pid"] = daemon.pid
    STATE.write_text(json.dumps(state, indent=2))
    wait_for(lambda: run(state, CLI, "--status", check=False).returncode == 0, 15, "the daemon to answer")


def cmd_stop(_args):
    if not STATE.exists():
        print("not running")
        return
    state = json.loads(STATE.read_text())
    if state["daemon_pid"] and pid_alive(state["daemon_pid"]):
        run(state, CLI, "--quit", check=False)
        deadline = time.monotonic() + 5
        while pid_alive(state["daemon_pid"]) and time.monotonic() < deadline:
            try:
                os.waitpid(state["daemon_pid"], os.WNOHANG)  # reap if it is our child
            except ChildProcessError:
                pass
            time.sleep(0.1)
        if pid_alive(state["daemon_pid"]):
            os.kill(state["daemon_pid"], signal.SIGKILL)
    for key in ("bus_pid", "xvfb_pid"):
        if state[key] and pid_alive(state[key]):
            os.kill(state[key], signal.SIGTERM)
    STATE.unlink()
    print(f"stopped (logs and screenshots kept in {SESSION})")


def cmd_cli(args):
    state = load_state()
    result = run(state, CLI, *args, check=False)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    sys.exit(result.returncode)


def status(state):
    out = run(state, CLI, "--status").stdout
    return dict(line.split(": ", 1) for line in out.splitlines() if ": " in line)


def cmd_status(_args):
    print(json.dumps(status(load_state()), indent=2))


def cmd_copy(args):
    state = load_state()
    text = " ".join(args)
    # xclip keeps serving the selection in the background, like the app that copied it.
    proc = subprocess.Popen(["xclip", "-selection", "clipboard", "-quiet"], stdin=subprocess.PIPE,
                            env=app_env(state), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=True, text=True)
    proc.stdin.write(text)
    proc.stdin.close()
    time.sleep(0.4)  # let the daemon see the XFixes event and store it


def cmd_clipboard(_args):
    print(run(load_state(), "xclip", "-selection", "clipboard", "-o").stdout)


def cmd_type(args):
    run(load_state(), "xdotool", "type", "--delay", "40", " ".join(args))
    time.sleep(0.3)  # search is debounced (100 ms)


def cmd_key(args):
    state = load_state()
    for key in args:
        run(state, "xdotool", "key", key)
        time.sleep(0.15)


def screenshot(state, name):
    from PIL import ImageGrab  # Pillow built with XCB grabs straight from the X server

    path = SESSION / "screenshots" / f"{name}.png"
    ImageGrab.grab(xdisplay=state["display"]).save(path)
    return path


def cmd_ss(args):
    print(screenshot(load_state(), args[0] if args else time.strftime("%H%M%S")))


def cmd_smoke(_args):
    """A real user flow, checked through the CLI status and the X clipboard."""
    cmd_start([])
    state = load_state()
    failures = []

    def check(condition, what):
        print(("ok    " if condition else "FAIL  ") + what)
        if not condition:
            failures.append(what)

    try:
        for text in ("alpha", "beta two", "gamma ray"):
            cmd_copy([text])
        s = status(state)
        check(s.get("backend") == "qt-clipboard" and s.get("backend_degraded") == "false",
              "complete X11 capture backend")
        check(s.get("items") == "3", "three copies captured from another app")

        run(state, CLI, "--toggle")
        time.sleep(0.5)
        check(status(state).get("visible") == "true", "--toggle shows the popup")
        print("      " + str(screenshot(state, "01-popup")))

        cmd_type(["beta"])
        print("      " + str(screenshot(state, "02-search")))
        cmd_key(["Return"])
        time.sleep(0.3)
        check(run(state, "xclip", "-selection", "clipboard", "-o").stdout == "beta two",
              "Enter copies the match back to the clipboard")
        check(status(state).get("visible") == "false", "Enter closes the popup")
        check(status(state).get("items") == "3", "copying back does not record a duplicate")

        run(state, CLI, "--toggle")
        time.sleep(0.5)
        cmd_key(["Down", "ctrl+g"])  # first copy -> "Assign group" dialog
        time.sleep(0.4)
        cmd_type(["Work/Servers"])
        cmd_key(["Return"])
        time.sleep(0.4)
        cmd_key(["Up", "Up"])  # back to the search box (Groups row, then search)
        cmd_key(["Right"])  # Right in the empty search box opens Groups
        cmd_key(["Down", "Right", "Down", "Right"])  # Work -> open, Servers -> open
        print("      " + str(screenshot(state, "03-groups")))
        cmd_key(["Escape"])
        time.sleep(0.3)
        check(status(state).get("visible") == "false", "Esc closes the popup")
    finally:
        cmd_stop([])
    if failures:
        die(f"{len(failures)} check(s) failed")
    print("smoke passed")


COMMANDS = {
    "start": cmd_start, "stop": cmd_stop, "cli": cmd_cli, "status": cmd_status, "copy": cmd_copy,
    "clipboard": cmd_clipboard, "type": cmd_type, "key": cmd_key, "ss": cmd_ss, "smoke": cmd_smoke,
}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        sys.exit(2)
    COMMANDS[sys.argv[1]](sys.argv[2:])
