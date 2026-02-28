#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Translate a Linux-style .config (via kconfiglib) into CMake cache data.

This script is intentionally conservative: it succeeds even when the
configuration inputs are missing so that developers can bootstrap the build
system before a full Kconfig tree exists.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kconfig", required=True, help="Path to the top-level Kconfig file")
    parser.add_argument("--config", required=True, help="Path to the .config file")
    parser.add_argument("--out", required=True, help="Destination CMake snippet")
    return parser.parse_args()


def _cmake_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace("\"", "\\\"")


def _write_minimal_output(output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        "# Auto-generated placeholder. Run menuconfig to populate symbols.\n"
        "set(AEROSYNC_KCONFIG_DEFINES)\n",
        encoding="utf-8",
    )


def _generate_cache(kconfig_path: Path, config_path: Path, output_path: Path) -> int:
    try:
        import kconfiglib  # type: ignore
    except ModuleNotFoundError:
        sys.stderr.write("kconfiglib is not installed.\n")
        return 1

    if not kconfig_path.is_file():
        sys.stderr.write(f"warning: Kconfig file '{kconfig_path}' not found; emitting empty cache.\n")
        _write_minimal_output(output_path)
        return 0

    kconf = kconfiglib.Kconfig(str(kconfig_path))
    if config_path.is_file():
        kconf.load_config(str(config_path))
    else:
        sys.stderr.write(
            f"warning: config file '{config_path}' not found; continuing with defaults.\n"
        )

    defines: dict[str, str] = {}
    cmake_sets: list[str] = []

    def set_define(name: str, value: str) -> None:
        defines[name] = value

    def emit_symbol(name: str, value: str, *, plain: bool) -> None:
        if plain:
            set_define(name, value)
        set_define(f"CONFIG_{name}", value)

    for sym in kconf.unique_defined_syms:
        if not sym.name:
            continue

        if sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE):
            tri = sym.tri_value
            if tri == 0:
                continue
            if tri == 2:
                emit_symbol(sym.name, "1", plain=True)
                cmake_sets.append(f"set(CONFIG_{sym.name} \"y\")")
            elif tri == 1:
                emit_symbol(sym.name, "m", plain=True)
                emit_symbol(f"{sym.name}_MODULE", "1", plain=True)
                cmake_sets.append(f"set(CONFIG_{sym.name} \"m\")")
            continue

        if sym.type in (kconfiglib.INT, kconfiglib.HEX, kconfiglib.STRING):
            value = sym.str_value
            if value in (None, ""):
                continue

            if sym.type == kconfiglib.STRING:
                emit_symbol(sym.name, f"\"{value}\"", plain=True)
            else:
                emit_symbol(sym.name, value, plain=True)

            cmake_sets.append(
                f"set(CONFIG_{sym.name} \"{_cmake_escape(value)}\")"
            )

    output_lines = [
        "# This file is auto-generated. Do not edit.",
        "set(AEROSYNC_KCONFIG_DEFINES",
    ]
    for name in sorted(defines):
        output_lines.append(f"    \"{_cmake_escape(name)}={_cmake_escape(defines[name])}\"")
    output_lines.append(")\n")
    output_lines.extend(cmake_sets)
    output_lines.append("\n")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(output_lines), encoding="utf-8")
    return 0


def main() -> int:
    args = _parse_args()
    kconfig_path = Path(args.kconfig)
    config_path = Path(args.config)
    output_path = Path(args.out)
    return _generate_cache(kconfig_path, config_path, output_path)


if __name__ == "__main__":
    raise SystemExit(main())
