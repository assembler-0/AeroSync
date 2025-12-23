#pragma once

/**
* VoidFrameX version header
* @scheme <EPOCH>.<FEATURE>.<PATCH>-<CODENAME>+branch.<GIT_DESCRIBE>+abi.<ABI_LEVEL>
* @version 1.0.1
* @compiler Clang
*/

#define VOIDFRAMEX_EPOCH 1
#define VOIDFRAMEX_FEATURE 0
#define VOIDFRAMEX_PATCH 1
#define VOIDFRAMEX_ABI_LEVEL 0
#define VOIDFRAMEX_ABI_LEVEL_STR "0"
#define VOIDFRAMEX_CODENAME "Invariant"

#define VOIDFRAMEX_VERSION "1.0.1-" VOIDFRAMEX_CODENAME "+branch.v1.0.1-11-gc0605e4-dirty.dev+abi." VOIDFRAMEX_ABI_LEVEL_STR

#define VOIDFRAMEX_COMPILER_VERSION __VERSION__
#define VOIDFRAMEX_DESCRIPTION "A modern monolithic operating system kernel designed for performance and modularity."
