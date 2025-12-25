#include <kernel/fkx/fkx.h>
#include <drivers/apic/pic.h>
#include <drivers/apic/apic.h>

const struct fkx_kernel_api *ic_kapi = NULL;

int ic_mod_init(const struct fkx_kernel_api *api) {
  if (!api) return -1;
  ic_kapi = api;
  ic_kapi->ic_register_controller(apic_get_driver());
  ic_kapi->ic_register_controller(pic_get_driver());
  return 0;
}

FKX_MODULE_DEFINE(
  ic,
  "0.0.1",
  "assembler-0",
  "APIC & PIC interrupt driver module",
  0,
  FKX_IC_CLASS,
  ic_mod_init,
  NULL
);