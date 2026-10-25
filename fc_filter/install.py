#!/usr/bin/env python3
"""Apply or verify the pinned FC patches in an existing source checkout; no downloads."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def install(firmware, checkout, mode):
    checkout = Path(checkout).resolve(strict=True)
    spec = json.loads((HERE / "manifest.json").read_text())[firmware]
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=checkout, text=True).strip()
    if revision != spec["commit"]:
        raise ValueError(f"Expected {spec['tag']} commit {spec['commit']}; got {revision}")
    # Verify all paths before modifying anything. Never silently adapt a patch
    # to another release or overwrite a local implementation.
    for relative in [*spec["before"], *spec["board_files"], *spec["copies"].values()]:
        if (checkout / relative).resolve() != checkout / relative:
            raise ValueError(f"Symlink in integration path: {relative}")
    for relative, expected in spec["board_files"].items():
        if digest(checkout / relative) != expected:
            raise ValueError(f"Board definition differs: {relative}")
    actual = {p: digest(checkout / p) for p in spec["before"]}
    if actual == spec["after"]:
        for src, dst in spec["copies"].items():
            if not (checkout / dst).is_file() or digest(checkout / dst) != digest(HERE / src):
                raise ValueError(f"Installed policy differs or is missing: {dst}")
        return {"firmware": firmware, "revision": revision, "status": "verified", "physical_port": 4}
    if mode == "verify":
        raise ValueError("FC integration is not installed exactly as pinned")
    if actual != spec["before"]:
        raise ValueError("Integration source files have local changes or an unsupported version")
    for dst in spec["copies"].values():
        if (checkout / dst).exists():
            raise ValueError(f"Refusing to overwrite existing file: {dst}")
    patch = HERE / "patches" / f"{firmware}.patch"
    subprocess.run(["git", "apply", "--check", str(patch)], cwd=checkout, check=True)
    if mode == "apply":
        subprocess.run(["git", "apply", str(patch)], cwd=checkout, check=True)
        for src, dst in spec["copies"].items():
            (checkout / dst).write_bytes((HERE / src).read_bytes())
        return install(firmware, checkout, "verify")
    return {"firmware": firmware, "revision": revision, "status": "ready", "physical_port": 4}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", choices=("inav", "ardupilot"))
    parser.add_argument("checkout", type=Path)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--apply", action="store_true", help="apply after checking revision, hashes and patch")
    group.add_argument("--verify", action="store_true", help="require an already installed, unmodified integration")
    args = parser.parse_args()
    try:
        mode = "apply" if args.apply else "verify" if args.verify else "check"
        print(json.dumps(install(args.firmware, args.checkout, mode), indent=2))
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print(f"FC integration refused: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
