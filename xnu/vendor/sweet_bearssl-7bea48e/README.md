# BearSSL subset (commit 7bea48e5e850ab4cafbe68d3765cdaba13a86d6f), as staged for the SWEET kernel

Exactly what `xnu/patches*/0020-stage-bearssl.sh` copies into `libkern/crypto/sweet_bearssl` (portable constant-time AES,
MD5, SHA-1, SHA-384/512 and their codecs; `src/inner.h` with the `machine/limits.h` include for KERNEL). Taken from the
staged tree of kernel CI run 41814562336 (xnu-buildtree artifact). BearSSL is MIT licensed (`LICENSE.txt`).

The stage script fetches the pinned commit from bearssl.org first and uses this copy only when that server does not
answer (it returned HTTP 502 for every CI run on 2026-11-04).
