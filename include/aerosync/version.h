#pragma once

/**
* AeroSync version header
* @scheme r<RELEASE>c<CANDIDATE> <EPOCH>.<FEATURE>.<PATCH>-<CODENAME>+branch.<GIT_DESCRIBE>[+abi.<ABI_LEVEL>]
* @version 2.2.3
*/

#define AEROSYNC_EPOCH 2
#define AEROSYNC_FEATURE 2
#define AEROSYNC_PATCH 3
#define AEROSYNC_ABI_LEVEL 0
#define AEROSYNC_CODENAME "Invariant"

#define AEROSYNC_VERSION "r0c1 - 2.2.3-" AEROSYNC_CODENAME "+branch.v1.0.1-88-ga8ba196-dirty.dev+abi." "0"
#define AEROSYNC_VERSION_LEAN  "r0c1-" AEROSYNC_CODENAME

#define AEROSYNC_COMPILER_VERSION __VERSION__
#define AEROSYNC_DESCRIPTION "A modern monolithic operating system kernel designed for performance and modularity."
