# NDS public header fixture

`ascend_nds.h` is an unmodified public API header from the user's UBSIO
`feat/nds-adapter-v2` checkout at commit
`6acfbb41bffc717206c5183b0737b45f9e9690cd`.
Its original Mulan PSL v2 copyright notice and license are retained.
SHA-256: `3f10e7de07d9379fddcd2e4f77f43b2b35dc5ab0d59bc5363d7592f8b1ac1557`.

CPU tests build `nds_test_backend.cpp` against this contract into an isolated
mock SDK directory. Production builds use the externally supplied NDS SDK;
neither this header nor the mock library is installed as part of TENT.
