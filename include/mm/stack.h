#pragma once

#include <kernel/types.h>

#define STACK_CANARY_VALUE 0xDEADBEEFCAFEBABE

extern uint64_t __stack_chk_guard;