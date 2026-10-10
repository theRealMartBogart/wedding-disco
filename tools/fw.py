#!/usr/bin/env python3
"""fw — FreeWili2 BSP task runner (cross-platform).

Commands:
  fw build [app]     configure+build an app for the RP2350B target
  fw run   [app]     load the RAM image AND stream its RTT (one session)  <-- use this
  fw flash [app]     load the RAM image and start it, then detach
  fw rtt             attach to a already-running image and stream RTT
  fw test            build+run host unit tests (CTest, no hardware)
  fw new-app <name>  scaffold apps/<name> from apps/template
Add --print to any command to print the command(s) instead of running.

The apps are RAM-only (pico_set_binary_type(no_flash)): nothing is ever written
to the QSPI flash, and the image is lost on power-cycle. Two consequences:
  * `fw flash` does NOT program flash despite the name -- it loads SRAM over SWD
    and starts the image by hand. See flash_command.
  * `fw rtt` resets the chip when it attaches, which throws a RAM-only image
    away. Prefer `fw run <app>`, which loads and streams from one attach.
"""
import argparse, pathlib, shutil, socket, subprocess, sys, time

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_APP = "wedding_disco"
OPENOCD_CFG = str(REPO_ROOT / "tools" / "openocd" / "freewili2.cfg")
RTT_PORT = 9090
# RP2350 SRAM is 0x20000000..0x20082000; scan the whole range for the RTT block.
RTT_SETUP = 'rtt setup 0x20000000 0x82000 "SEGGER RTT"'

def build_command(app):
    return ["cmake", "--build", "--preset", "target", "--target", app]

def _openocd():
    """(exe, scripts_dir) for the Pico-SDK OpenOCD. Uses the ~/.pico-sdk install
    (newest version) when present — matching how subghz flashes — otherwise falls
    back to `openocd` on PATH with its built-in scripts (scripts_dir = None)."""
    root = pathlib.Path.home() / ".pico-sdk" / "openocd"
    if root.is_dir():
        exe_name = "openocd.exe" if sys.platform == "win32" else "openocd"
        for ver in sorted(root.iterdir(), reverse=True):
            exe, scripts = ver / exe_name, ver / "scripts"
            if exe.exists():
                return str(exe), (str(scripts) if scripts.is_dir() else None)
    return "openocd", None

def _openocd_base():
    exe, scripts = _openocd()
    cmd = [exe]
    if scripts:
        cmd += ["-s", scripts]
    return cmd + ["-f", OPENOCD_CFG]

def _find_elf(app):
    """The product apps build under src/; find the .elf wherever CMake put it."""
    matches = sorted(REPO_ROOT.glob(f"build/**/{app}.elf"))
    return (matches[0].relative_to(REPO_ROOT).as_posix() if matches
            else f"build/src/{app}.elf")

def elf_entry(elf_path):
    """e_entry straight out of the ELF32 header (offset 24, little-endian).

    Read here rather than shelled out to arm-none-eabi-readelf so `fw flash`
    works without the cross toolchain on PATH."""
    with open(REPO_ROOT / elf_path, "rb") as f:
        head = f.read(28)
    if head[:4] != b"\x7fELF":
        raise ValueError(f"{elf_path} is not an ELF file")
    return int.from_bytes(head[24:28], "little")

def flash_command(app):
    """Load a RAM-only (no_flash) image into SRAM and start it.

    These apps are built with pico_set_binary_type(no_flash): the image exists
    only in SRAM and the QSPI flash is never touched. That makes the usual
    `program <elf> verify reset exit` actively wrong — the trailing reset drops
    the chip back into the bootrom, which knows nothing about an image that was
    never written to flash, so the board would come up running whatever is
    still in flash instead. Nothing would appear to fail.

    So instead: halt, load, then start the image by hand.

    Core 0 starts at the ELF entry (_entry_point), not at the vector table's
    _reset_handler: _entry_point is the one that sets VTOR, loads MSP from the
    vector table and jumps. _reset_handler assumes it was reached by a real
    hardware reset and sends non-core0 back into the bootrom.

    Nothing else is allowed to run on core 0 between `reset halt` and that
    resume, and that is the subtle part. _entry_point's first instructions
    enable the FPU via CPACR and immediately execute FP instructions, which
    only works from the pristine Secure state a reset leaves behind. An earlier
    version of this resumed core 1 first to park it in the bootrom's wait loop;
    that also let the bootrom run on core 0 (its PC moved 0xa0 -> 0x318e), and
    core 0 then HardFaulted the instant it was resumed into _entry_point.
    Resuming core 0 alone releases core 1 as well, and the app's own
    multicore_launch_core1() is happy with that.

    There is deliberately no verify_image step. OpenOCD verifies by uploading a
    small CRC routine and *running it on the target*, which for a RAM-only
    image means scribbling over the very memory we just loaded (and it leaves
    core 0 running, so the resume then fails with "not halted"). The
    load_image byte count is the check we get.
    """
    return _openocd_base() + _load_cmds(app) + ["-c", "shutdown"]

def _load_cmds(app):
    """The OpenOCD -c sequence that puts a RAM image in SRAM and starts it."""
    elf = _find_elf(app)
    entry = elf_entry(elf)
    return [
        "-c", "init",
        "-c", "reset halt",
        "-c", f"load_image {elf}",
        "-c", "targets rp2350.cm0",
        "-c", f"resume 0x{entry:08x}",
    ]

def run_command(app):
    """Load a RAM image AND stream its RTT, in a single OpenOCD session.

    This is the command to use for RAM-only builds. Attaching a *second* time
    is not harmless here: OpenOCD's `init` resets the chip on this target
    (verified 2026-08-07 — after `fw flash`, a plain `init` attach leaves the
    PC at 0x3ec, in the bootrom). A reset costs a flash-resident app nothing
    because the bootrom just runs it again from flash, but a RAM-only image
    exists nowhere the bootrom can find, so it is simply gone. Loading and
    serving RTT from one attach never resets in between.
    """
    return _openocd_base() + _load_cmds(app) + [
        "-c", RTT_SETUP, "-c", "rtt start",
        "-c", f"rtt server start {RTT_PORT} 0"]

def rtt_command():
    return _openocd_base() + [
        "-c", "init", "-c", RTT_SETUP, "-c", "rtt start",
        "-c", f"rtt server start {RTT_PORT} 0"]

def _host_toolchain_args():
    """Extra `cmake` configure args that pin a host C compiler + Ninja for the
    standalone tests/ tree (no Pico SDK, no cross-compiler). Returns [] entries
    that are simply omitted when a tool can't be found, so CMake falls back to
    its own defaults (e.g. system cc/gcc, non-Ninja generator).
    """
    args = []
    if sys.platform == "win32":
        # Mirrors the subghz repo's proven host-test toolchain: MSYS2 MinGW
        # GCC + the Ninja bundled with the Pico SDK VS Code extension.
        gcc = pathlib.Path("C:/msys64/mingw64/bin/gcc.exe")
        if gcc.exists():
            args += [f"-DCMAKE_C_COMPILER={gcc}"]
        ninja_root = pathlib.Path.home() / ".pico-sdk" / "ninja"
        ninja = next(iter(sorted(ninja_root.glob("*/ninja.exe"), reverse=True)), None) \
            if ninja_root.is_dir() else None
        if ninja is not None:
            args += ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"]
    else:
        # Non-Windows: trust the default host cc/gcc; use Ninja if it's on
        # PATH, otherwise let CMake pick its default generator (e.g. Make).
        if shutil.which("ninja"):
            args += ["-G", "Ninja"]
    return args

def test_command():
    tests_dir = REPO_ROOT / "tests"
    build_dir = REPO_ROOT / "build-tests"
    configure = ["cmake", "-S", str(tests_dir), "-B", str(build_dir)]
    configure += _host_toolchain_args()
    return [
        configure,
        ["cmake", "--build", str(build_dir)],
        ["ctest", "--test-dir", str(build_dir), "--output-on-failure"],
    ]

def new_app(name, repo_root=REPO_ROOT):
    src = pathlib.Path(repo_root) / "apps" / "template"
    dest = pathlib.Path(repo_root) / "apps" / name
    if dest.exists():
        raise FileExistsError(dest)
    shutil.copytree(src, dest)
    cml = dest / "CMakeLists.txt"
    cml.write_text(cml.read_text().replace("template", name))
    return dest

def _run(cmds, do_print):
    if isinstance(cmds[0], str):
        cmds = [cmds]
    for c in cmds:
        if do_print:
            print(" ".join(c))
        else:
            subprocess.run(c, cwd=REPO_ROOT, check=True)

def run_rtt(seconds=0, cmd=None):
    """Stream OpenOCD's RTT channel 0 to stdout. seconds=0 runs until Ctrl+C;
    seconds>0 exits after that window (for scripted checks). Diagnostics on the
    FreeWili2 are RTT-only.

    `cmd` defaults to a plain attach (rtt_command). Pass run_command(app) to
    load a RAM image and stream it from the same session — see run_command for
    why a RAM-only build cannot survive a separate attach."""
    proc = subprocess.Popen(cmd or rtt_command(), cwd=REPO_ROOT,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(2)  # let OpenOCD attach and locate the RTT control block
        if proc.poll() is not None:
            print("openocd exited early — is the debug probe connected?", file=sys.stderr)
            return 1
        try:
            sock = socket.create_connection(("127.0.0.1", RTT_PORT), timeout=5)
        except OSError as e:
            print(f"could not connect to RTT server on {RTT_PORT}: {e}", file=sys.stderr)
            return 1
        sock.settimeout(0.5)
        deadline = time.time() + seconds if seconds > 0 else None
        print(f"--- RTT connected (port {RTT_PORT}); Ctrl+C to stop ---", file=sys.stderr)
        while deadline is None or time.time() < deadline:
            try:
                data = sock.recv(4096)
                if not data:
                    break
                sys.stdout.write(data.decode("ascii", "replace"))
                sys.stdout.flush()
            except socket.timeout:
                pass
    except KeyboardInterrupt:
        pass
    finally:
        try:
            sock.close()
        except NameError:
            pass
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
    return 0

def main(argv=None):
    p = argparse.ArgumentParser(prog="fw")
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("build", "flash"):
        sp = sub.add_parser(name); sp.add_argument("app", nargs="?", default=DEFAULT_APP)
        sp.add_argument("--print", dest="show", action="store_true")
    sp = sub.add_parser("run", help="load a RAM image and stream its RTT (one session)")
    sp.add_argument("app", nargs="?", default=DEFAULT_APP)
    sp.add_argument("--print", dest="show", action="store_true")
    sp.add_argument("-s", "--seconds", type=int, default=0,
                    help="capture for N seconds then exit (0 = until Ctrl+C)")
    sp = sub.add_parser("rtt")
    sp.add_argument("--print", dest="show", action="store_true")
    sp.add_argument("-s", "--seconds", type=int, default=0,
                    help="capture for N seconds then exit (0 = until Ctrl+C)")
    sp = sub.add_parser("test"); sp.add_argument("--print", dest="show", action="store_true")
    sp = sub.add_parser("new-app"); sp.add_argument("name")

    a = p.parse_args(argv)
    if a.cmd == "build":   _run(build_command(a.app), a.show)
    elif a.cmd == "flash": _run(flash_command(a.app), a.show)
    elif a.cmd == "run":
        if a.show:
            _run(run_command(a.app), True)
        else:
            return run_rtt(a.seconds, run_command(a.app))
    elif a.cmd == "rtt":
        # NOTE: attaching resets the chip, which kills a RAM-only app. This is
        # only useful for a flash-resident image; use `fw run <app>` otherwise.
        if a.show:
            _run(rtt_command(), True)
        else:
            return run_rtt(a.seconds)
    elif a.cmd == "test":  _run(test_command(), a.show)
    elif a.cmd == "new-app":
        print("created", new_app(a.name))
    return 0

if __name__ == "__main__":
    sys.exit(main())
