#pragma once

/**
* VoidFrameX version header
* @scheme <EPOCH>.<FEATURE>.<PATCH>[-dev.<git-hash>.<date>]
* @version 0.0.1
* @compiler Clang
*/

#define VOIDFRAMEX_EPOCH 0
#define VOIDFRAMEX_FEATURE 0
#define VOIDFRAMEX_PATCH 1
#define VOIDFRAMEX_VERSION "0.0.1"

#define VOIDFRAMEX_ABI_LEVEL 0

#define VOIDFRAMEX_BUILD_DATE __DATE__ " " __TIME__
#define VOIDFRAMEX_COMPILER_VERSION __VERSION__
#define VOIDFRAMEX_CODENAME "" // No codename yet
#define VOIDFRAMEX_DESCRIPTION "a modern 64-bit x86_64 limine-based monolithic kernel."