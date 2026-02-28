# cmdline args
> this contains all the cmdline fields and their descriptions (im trying to keep this up-to-date as possible but most of the time im too lazy lmfao)

- `quiet`: disable all output (still lives in buffer)
- `verbose`: enable verbose output
- `mtest`: do memory tests (depends on CONFIG_INCLUDE_MM_TEST)
- `rcutest`: do rcu, percpu tests (depends on CONFIG_RCU_PERCPU_TEST)
- `dumpdevtree`: dump the device tree
- `nosmp`: disable SMP (depends on CONFIG_SYMETRIC_MP)
- `initrd=VAL`: VAL is path to initrd
- `bootinfo`: display some limine boot info
- `kaslrinfo`: logs kernel base
- `mm_page_lvl`: log cmdline
- `fwinfo`: log info from smbio
- `acpi_enum`: enumerate and log acpi bus
- `vfstest`: run vfs tests (depends on CONFIG_VFS_TESTS)
- `disable-mm-scrubber`: disable mm_scrubberd thread (depends on MM_HARDENING)