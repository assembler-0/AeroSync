#pragma once

/**
 * Deferred statement implementation for C.
 * copy-pasted from https://antonz.org/defer-in-c/
 */

#define _DEFER_CONCAT(a, b) a##b
#define _DEFER_NAME(a, b) _DEFER_CONCAT(a, b)

// Deferred function and its argument.
struct _defer_ctx {
    void (*func)(void*);
    void* arg;
};

// Calls the deferred function with its argument.
static inline void _defer_cleanup(struct _defer_ctx* ctx) {
    if (ctx->func) ctx->func(ctx->arg);
}

// Create a deferred function call for the current scope.
#define defer(fn, ptr)                                      \
    struct _defer_ctx _DEFER_NAME(_defer_var_, __COUNTER__) \
        __attribute__((cleanup(_defer_cleanup))) =          \
            {(void (*)(void*))(fn), (void*)(ptr)}