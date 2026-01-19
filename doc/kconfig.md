# AeroSync Kconfig Workflow

## Prerequisites
- Python ≥ 3.8 (detected automatically by CMake)
- `kconfiglib` installed in the selected Python environment (`pip install kconfiglib`)
- A top-level `Kconfig` file describing the kernel configuration menu

## Generating and Using `.config`
1. Configure the build directory as usual, e.g. `cmake -S . -B build`.
2. Launch the interactive configurator with `cmake --build build --target menuconfig` (or `ninja menuconfig`).
   - The UI writes the resulting `.config` to the repository root by default; set `-DAEROSYNC_CONFIG=/path/to/.config` to override.
3. Re-run CMake (or rely on the auto reconfigure) so the `.config` is parsed by `tools/kconfig_to_cmake.py`.
4. All enabled symbols become `CONFIG_*` compile definitions on `aerosync.krnl`, matching the selections in `.config`.

## Notes
- CMake fails fast if Python or `kconfiglib` are missing to keep builds deterministic.
- The generated cache lives under `<build>/generated/kconfig_cache.cmake`; delete it (or the entire build directory) to force regeneration.

## Symbol → Define Invariants
- Every Kconfig symbol `FOO` is exported as the compile definition `CONFIG_FOO=<value>`.
- Bool/tri-state symbols also receive the legacy alias `FOO=<value>` so existing `#ifdef FOO` checks remain valid.
- For modules, both `CONFIG_FOO=m` and `FOO_MODULE=1` are emitted.
- Integer/hex/string symbols preserve their literal value in both `CONFIG_FOO` and `FOO` (quotes stripped), ensuring there is a single canonical mapping from menu selections to C preprocessor names.
