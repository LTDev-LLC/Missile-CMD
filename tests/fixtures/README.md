# Compatibility fixtures

`run_v1.dat` is the independent legacy checkpoint fixture retained from the existing tests.

`pre-sharing.bin` was captured with `tests/test_compatibility.c` compiled against the preserved source baseline before this shared-logic refactor (63,156-byte FAP, 2026-09-11). It contains settings, all difficulty score boards, 45 mode/difficulty run traces at three points, exact-wave starting data where supported, state hashes, and the resulting profile. Normal tests only read it and compare every byte. Do not regenerate it from the current implementation to fix a regression.
