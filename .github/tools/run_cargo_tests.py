#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Discover and run per-component cargo recipes.

Not IDF host tests. IDF's host target still uses ``idf.py`` and Unity; that
pipeline stays in ``build_and_run_apps.yml``. This runner is for crates that
call ``cargo test`` / ``cargo fmt`` / ``cargo clippy`` on the GitHub runner
itself, with no IDF image and no flash/monitor.

A component opts in with a fixed layout::

    <component>/ci/cargo/run.sh

next to ``idf_component.yml``. The script is the recipe (which packages to
test, extra crate-local checks). This runner only finds those files, runs
each from the component directory, and reports every failure at the end.

The GitHub workflow installs a shared Rust toolchain once. Extra apt/pip
needs belong inside ``ci/cargo/run.sh``.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

# <component>/ci/cargo/run.sh → component is parents[2]
CARGO_RECIPE = Path("ci") / "cargo" / "run.sh"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def discover(root: Path) -> list[Path]:
    scripts: list[Path] = []
    for manifest in sorted(root.glob("*/idf_component.yml")):
        script = manifest.parent / CARGO_RECIPE
        if script.is_file():
            scripts.append(script)
    return scripts


def component_dir(script: Path) -> Path:
    return script.parents[2]


def run_one(script: Path, root: Path) -> int:
    component = component_dir(script)
    in_gha = bool(os.environ.get("GITHUB_ACTIONS"))
    if in_gha:
        print(f"::group::{component.name}")
    print(f"$ {script.relative_to(root)}", flush=True)
    if not os.access(script, os.X_OK):
        print(
            f"error: {script} is not executable; chmod +x it so CI and a local "
            "run use the same path",
            file=sys.stderr,
        )
        if in_gha:
            print("::endgroup::")
        return 1
    result = subprocess.run([str(script)], cwd=component)
    if in_gha:
        print("::endgroup::")
    if result.returncode:
        msg = f"{component.name} cargo tests failed (exit {result.returncode})"
        print(f"::error::{msg}" if in_gha else f"error: {msg}")
    return result.returncode


def main() -> int:
    # Line-buffer so GitHub Actions `::group::` wraps cargo output, not just
    # our own prints after the child has already streamed everything.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)
        sys.stderr.reconfigure(line_buffering=True)

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=repo_root(),
        help="Repository root (default: inferred from this file's location)",
    )
    args = parser.parse_args()
    root = args.root.resolve()

    scripts = discover(root)
    if not scripts:
        print(f"No <component>/{CARGO_RECIPE.as_posix()} found; nothing to run")
        return 0

    print(f"Cargo tests: {len(scripts)} component(s)")
    failed: list[str] = []
    for script in scripts:
        if run_one(script, root) != 0:
            failed.append(component_dir(script).name)

    if failed:
        print("Failed:", ", ".join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
