#pragma once

/**
 * AeroSync version header
 * @scheme r<RELEASE>c<CANDIDATE> <EPOCH>.<FEATURE>.<PATCH>-<CODENAME>+branch.<GIT_DESCRIBE>[+abi.<ABI_LEVEL>]
 * @version 3.2.5
*/

#define AEROSYNC_EPOCH 3
#define AEROSYNC_FEATURE 2
#define AEROSYNC_PATCH 5
#define AEROSYNC_ABI_LEVEL 0
#define AEROSYNC_CODENAME "Invariant"

#ifdef MM_HARDENING
# define AEROSYNC_VERSION "r0c1.hardened - 3.2.5-" AEROSYNC_CODENAME "+branch.v1.0.1-101-gc10384d-dirty.dev+abi." "0"
# define AEROSYNC_VERSION_LEAN  "r0c1.hardened-" AEROSYNC_CODENAME
#else /* MM_HARDENING */
# define AEROSYNC_VERSION "r0c1 - 3.2.5-" AEROSYNC_CODENAME "+branch.v1.0.1-101-gc10384d-dirty.dev+abi." "0"
# define AEROSYNC_VERSION_LEAN  "r0c1-" AEROSYNC_CODENAME
#endif /* MM_HARDENING */

#define AEROSYNC_TARGET __TARGET__
#define AEROSYNC_COMPILER_VERSION __VERSION__
#define AEROSYNC_DESCRIPTION "A modern monolithic operating system kernel designed for performance and modularity."
