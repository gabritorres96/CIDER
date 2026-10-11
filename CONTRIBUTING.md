# Contributing to ORVYN

## Ground rules

1. Real, compilable code only. No pseudocode, no simulated behaviour, no fake test results.
2. Do not claim support for an API, filesystem or application until a reproducible test proves it.
3. Keep modules separate: analysis (`macho`, `corefoundation`, `bundle`, `dmg`, `analysis`) must
   never depend on a loader/runtime, and nothing in analysis may execute untrusted code.
4. Mach-O files, plists and disk images are **untrusted input**: bounds-check everything, use the
   overflow-safe helpers in `orvyn/bytes.hpp` (`checked_add`, `checked_mul`, `ByteView`).
5. Do not copy code from Wine, Darling, GNUstep, 7-Zip or Apple sources without checking the
   licence first (see [docs/LICENSES.md](docs/LICENSES.md)). Never add Apple proprietary
   frameworks or headers to the repository.
6. Every fixed bug gets a regression test.

## Workflow

```bash
cmake -S . -B build -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

* Add tests next to the feature (`tests/unit`, `tests/integration`, `tests/regression`).
  Fixtures are generated in code (`tests/fixtures/*.hpp`); do not commit binaries.
* Warnings are treated seriously (`/W4`): the tree builds warning-free.
* Update `docs/PROGRESS.md` and the relevant document in the same change.

## Adding a load command

1. Add the constant and parser in `src/macho/macho.cpp` with a minimum-size check.
2. Validate every offset/size against the slice (`check_range` / `check_table`).
3. Add the field to `include/orvyn/macho.hpp`, to the builder in `tests/fixtures/macho_builder.hpp`,
   a positive test in `tests/unit/test_macho*.cpp` and negative cases in `test_malformed.cpp`.
4. Document it in `docs/MACHO.md`.

## Adding a CLI command

Implement it in `src/cli/commands.cpp`, wire it in `main.cpp`, give it `--json` output, an exit
code consistent with the table in the README and an integration test in
`tests/integration/test_cli.cpp`. Commands that depend on missing capabilities must say so.

## Security reports

See [SECURITY.md](SECURITY.md).
