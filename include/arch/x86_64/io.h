#pragma once

#include <aerosync/types.h>
#include <compiler.h>

#ifdef __cplusplus
extern "C" {

#endif

static __finline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static __finline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
  return ret;
}

static __finline void outw(uint16_t port, uint16_t val) {
  __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static __finline uint16_t inw(uint16_t port) {
  uint16_t ret;
  __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
  return ret;
}

static __finline void outl(uint16_t port, uint32_t val) {
  __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static __finline uint32_t inl(uint16_t port) {
  uint32_t ret;
  __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
  return ret;
}

static __finline void outsb(uint16_t port, void* buf, size_t len) {
  __asm__ volatile ("cld; rep outsb" : "+S"(buf), "+c"(len) : "d"(port) : "memory");
}

static __finline void insb(uint16_t port, void* buf, size_t len) {
  __asm__ volatile ("cld; rep insb" : "+D"(buf), "+c"(len) : "d"(port) : "memory");
}

static __finline void outsl(uint16_t port, void* buf, size_t len) {
  __asm__ volatile ("cld; rep outsl" : "+S"(buf), "+c"(len) : "d"(port) : "memory");
}

static __finline void insl(uint16_t port, void* buf, size_t len) {
  __asm__ volatile ("cld; rep insl" : "+D"(buf), "+c"(len) : "d"(port) : "memory");
}

static __finline void outsw(uint16_t port, void* buf, size_t len) {
  __asm__ volatile ("cld; rep outsw" : "+S"(buf), "+c"(len) : "d"(port) : "memory");
}

static __finline void insw(uint16_t port, void* buf, size_t len) {
  __asm__ volatile ("cld; rep insw" : "+D"(buf), "+c"(len) : "d"(port) : "memory");
}

static __finline void io_wait(void) {
  outb(0x80, 0);
}

#ifdef __cplusplus
}
#endif
