# NDS public header fixture

`ascend_nds.h` is the public API header from the user's UBSIO
`feat/nds-adapter-v2` checkout at commit
`6acfbb41bffc717206c5183b0737b45f9e9690cd`, with only the copyright year
updated to 2026. Its original Mulan PSL v2 license text is retained.
SHA-256: `8d88cfb991bb3a5dfe33d2f4194443bbe36b3aac04f0e589d1e9abef0986a232`.

CPU tests build `nds_test_backend.cpp` against this contract into an isolated
mock SDK directory. Production builds use the externally supplied NDS SDK;
neither this header nor the mock library is installed as part of TENT.
