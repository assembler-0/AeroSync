# [CONTRIBUTING.md](CONTRIBUTING.md) - coding guidelines to AeroSync by [assembler-0](https://github.com/assembler-0)

## Introduction
Firstly, thank you for considering contributing to AeroSync! As usual, to maintain a healthy community, code quality and a good development experience, there are a few guidelines that you should follow.

As you already know, AeroSync is a modern, real hardware-capable kernel, written in mostly C and assembly (x86_64 nasm flavor). It is heavily inspired by the Linux, BSD and XNU (Mach) kernels.
Therefore, I want this to be a bit more of a serious project (imo), not just a *just-work* implementation. This project is open to the community, but architecturally driven by the maintainers.
The project's final goal is to be a General-Purpose (GP) kernel for x86_64-compatible CPUs (other architectures and kernel flavors are not planned whatsoever). 
One more time, this is a kernel, not an os or trying to be an os, I have seen projects that claim to be a *kernel* or an *os*, but architecturally, 
they are an **os-in-kernel**-which is the worst possible thing.

## Philosophy
- Use **proven** designs from major kernels.
- Don't **reinvent the wheel**.
- Must be **correct**.
- Must be **secure**.
- Must be **fast**.
- Must be **abstracted**.
- And most importantly, **human-readable**. (I have seen too many projects (including the one you are using right now) that are not readable at all)

## Goals & Non-Goals
### Goals
- is a GP kernel
- is real-hardware-capable
- is (very) modular
- is fast
- is secure
- is easy to extend
- is configurable
- is (architecturally) beautiful
- is backwards-compatible
- is (human) understandable and readable
- is documented
- is up-to-date
- is correct
### Non-Goals (any changes involving these will be rejected without further discussion)
- is an os
- rapid feature accretion
- random rewrites (e.g., because "Rust/C++/ABC is better than C/assembly")
- toy kernel
- constant ABI changes
- user-first approach
- Linux clone (even though it is a good idea, let's keep things separated)
- *"just-work"* implementations

## Communication
- Main update channel: [Discord](https://discord.com/channels/440442961147199490/1452605767185338378) (osdev server)
- Email: diaviekone13@gmail.com (main), assembler[697-700]@gmail.com (backup/alt, inclusive of the former) 
- Zalo: +84908032198
- Github: [assembler-0](https://github.com/assembler-0)
- Facebook: [assembler0](https://www.facebook.com/the.assembly.19052012)
- Primary language for development: English (though Vietnamese is my mother tongue, I prefer using English for kernel development for more expressive communication)
- Timezone: Ho Chi Minh City (HCMC) (GMT+7) 
- Final decision-maker: owner and maintainer(s) of the repository 

## Contribution process
1. fork
2. make changes
3. submit a pull request
4. wait for review
5. merge
6. cycle
- one logical change per PR (please leave a note if the PR contains multiple changes).
- drafts are welcome.

## Standards (ALL changes must follow these standards)
- ISO C11 standard (GNU extensions allowed where justified).
- 2-space indentation.
- no tabs.
- CMake build system.
- LLVM infrastructure only.
- rc versioning scheme. (see [version.h.in](include/aerosync/version.h.in) for more details).
- Both C-/C++-style comments are allowed. (// ... and /* ... */)

## Code style
- naming:
  - types:
    - struct names: snake_case
    - typedef struct names: snake_case_t or snake_case
  - variables: snake_case
  - functions: snake_case
  - macros: SCREAMINg_SNAKE_CASE or snake_case
- use of allocators:
  - kmalloc & kmem_cache: for cache pools and object allocations smaller than 128KiB.
  - vmalloc: for virtually contiguous allocations.
  - alloc_pages: for physical contiguous allocations. (power-of-two)
  - pmm_alloc_pages: legacy wrapper for alloc_pages.
- memory:
  - prefer larger contiguous allocations when possible.
  - use the correct allocators.
  - use the correct alignment.
  - should add bound/null checks where applicable.

## How to get merged
- Runs on major emulators. (QEMU, Bochs, VirtualBox, VMware, etc.)
- Tested on at least one real machine.
- Passes all tests.
- Changes are documented.
- Changes follow the code style guidelines in this document.
- Changes are correct, fast and secure.

## Legal and Licensing
- Declare external sources if used
- You can use code from other projects as long as they are compliant with the kernel's license
- no proprietary code allowed (including leaked code from proprietary projects, e.g., Windows NT4.0 source leak)
- all contributions are under the project's license 
## Credits
- Contributors will be acknowledged ([CONTRIBUTORS.md](CONTRIBUTORS.md)) accordingly.