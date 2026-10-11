#!/usr/bin/env python3
"""Stage and verify a local macOS bundle without installing it."""

import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import sys


REPO = Path(__file__).resolve().parent.parent
MACH_O_MAGIC = {
    bytes.fromhex(value)
    for value in ("cffaedfe", "cefaedfe", "feedfacf", "feedface",
                  "cafebabe", "bebafeca", "cafebabf", "bfbafeca")
}


def macho_files(bundle):
    for path in bundle.rglob("*"):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as binary:
                if binary.read(4) in MACH_O_MAGIC:
                    yield path


def run(command, log):
    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)


def require_staged_app_closed(bundle):
    executable = (bundle / "Contents/MacOS/Moonlight").resolve()
    libproc = ctypes.CDLL("/usr/lib/libproc.dylib")
    libproc.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    libproc.proc_pidpath.restype = ctypes.c_int
    pids = subprocess.check_output(["ps", "-axo", "pid="], text=True).split()
    for pid in pids:
        path = ctypes.create_string_buffer(4096)
        if libproc.proc_pidpath(int(pid), path, len(path)) > 0:
            if Path(os.fsdecode(path.value)).resolve() == executable:
                raise RuntimeError(f"Staged Moonlight is running (PID {pid}); close it normally before staging")


def audit_dependencies(bundle, log_path):
    frameworks = bundle / "Contents/Frameworks"
    executable_dir = bundle / "Contents/MacOS"
    files = list(macho_files(bundle))
    errors = []
    with log_path.open("w") as log:
        for binary in files:
            output = subprocess.check_output(["otool", "-L", str(binary)], text=True)
            log.write(output + "\n")
            for line in output.splitlines():
                if not line.startswith("\t"):
                    continue
                dependency = line.strip().split(" (", 1)[0]
                if dependency.startswith(("/System/Library/", "/usr/lib/")):
                    continue
                locations = {
                    "@loader_path/": binary.parent,
                    "@executable_path/": executable_dir,
                    "@rpath/": frameworks,
                }
                target = next((directory / dependency.removeprefix(prefix)
                               for prefix, directory in locations.items()
                               if dependency.startswith(prefix)), None)
                if target is None or not target.exists():
                    errors.append(f"{binary.relative_to(bundle)}: {dependency}")
    if errors:
        raise RuntimeError("Unbundled runtime dependencies:\n" + "\n".join(errors))
    return len(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-app", type=Path,
                        default=REPO / "build/mac-client/app/Moonlight.app")
    parser.add_argument("--qt-dir", type=Path,
                        default=REPO / "build/vrr-hybrid/qt/6.11.1/macos")
    parser.add_argument("--deploy-dir", type=Path, default=REPO / "build/mac-deploy",
                        help="Staging directory; use a separate directory while another build is running")
    args = parser.parse_args()
    source = args.source_app.resolve()
    deploy_dir = args.deploy_dir.resolve()
    bundle = deploy_dir / "Moonlight.app"
    deploy_tool = args.qt_dir.resolve() / "bin/macdeployqt"
    if not (source / "Contents/MacOS/Moonlight").is_file():
        raise RuntimeError(f"Missing built application: {source}")
    if source == bundle:
        raise RuntimeError("Source must be the build bundle, not the staged bundle")
    if not deploy_tool.is_file():
        raise RuntimeError(f"Missing Qt deployment tool: {deploy_tool}")
    require_staged_app_closed(bundle)
    deploy_dir.mkdir(parents=True, exist_ok=True)
    with (deploy_dir / "macdeployqt.log").open("w") as log:
        run(["ditto", str(source), str(bundle)], log)
        # Sign explicitly below, after excluding the SDK's unused Mimer driver.
        run([str(deploy_tool), str(bundle), f"-qmldir={REPO / 'app/gui'}",
             "-appstore-compliant", "-no-codesign"], log)

    mimer = bundle / "Contents/PlugIns/sqldrivers/libqsqlmimer.dylib"
    if mimer.exists():
        # This optional driver requires a separately installed Mimer database.
        # Moonlight uses neither Mimer nor that external database runtime.
        excluded = deploy_dir / "excluded-plugins"
        excluded.mkdir(exist_ok=True)
        mimer.replace(excluded / mimer.name)

    main_binary = bundle / "Contents/MacOS/Moonlight"
    with (deploy_dir / "codesign.log").open("w") as log:
        for binary in macho_files(bundle):
            if binary != main_binary:
                run(["codesign", "--force", "--sign", "-", str(binary)], log)
        frameworks = sorted(bundle.rglob("*.framework"),
                            key=lambda path: len(path.parts), reverse=True)
        for framework in frameworks:
            run(["codesign", "--force", "--sign", "-", str(framework)], log)
        # Signing the executable directly treats it as its containing app.
        # The application must therefore be signed after all nested code.
        run(["codesign", "--force", "--sign", "-", str(bundle)], log)
        run(["codesign", "--verify", "--deep", "--strict", "--verbose=2",
             str(bundle)], log)

    count = audit_dependencies(bundle, deploy_dir / "otool-all.log")
    environment = os.environ.copy()
    for name in ("DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH",
                 "DYLD_FALLBACK_LIBRARY_PATH", "QT_QPA_PLATFORM", "QT_PLUGIN_PATH",
                 "QML2_IMPORT_PATH", "QML_IMPORT_PATH"):
        environment.pop(name, None)
    help_result = subprocess.run([str(main_binary), "--help"], env=environment,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, timeout=30, check=True)
    (deploy_dir / "help-smoke.log").write_text(help_result.stdout)
    if "Usage:" not in help_result.stdout:
        raise RuntimeError("Staged application returned no usage output")
    print(f"Staged {bundle}")
    print(f"Verified signatures, {count} bundled Mach-O dependencies, and --help")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print(f"Mac staging failed: {error}", file=sys.stderr)
        sys.exit(1)
