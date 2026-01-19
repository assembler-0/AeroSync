#pragma once

#include <aerosync/types.h>
#include <lib/printk.h>
#include <aerosync/classes.h>
#include <aerosync/panic.h>

/* Lockdep stubs */
#define lockdep_is_held(lock) (1)

static inline bool check_data_corruption(bool v) { return v; }
#define CHECK_DATA_CORRUPTION(condition, addr, fmt, ...)		 \
  check_data_corruption(({				                         	 \
    bool corruption = unlikely(condition);	             		 \
    if (corruption) {				                               	 \
        panic(fmt, ##__VA_ARGS__);			                 		 \
    }				                                            		 \
    corruption;		                                     			 \
  }))