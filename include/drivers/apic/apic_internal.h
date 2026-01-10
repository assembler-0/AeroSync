#pragma once
#include <aerosync/types.h>

struct apic_ops {
    const char *name;
    int (*init_lapic)(void);
    void (*send_eoi)(uint32_t irn);
    void (*send_ipi)(uint32_t dest, uint8_t vector, uint32_t mode);
    uint32_t (*get_id)(void);
    void (*timer_init)(uint32_t freq_hz);
    void (*timer_set_frequency)(uint32_t freq_hz);
    void (*shutdown)(void);
    uint32_t (*read)(uint32_t reg);
    void (*write)(uint32_t reg, uint32_t val);
};

extern const struct apic_ops xapic_ops;
extern const struct apic_ops x2apic_ops;
