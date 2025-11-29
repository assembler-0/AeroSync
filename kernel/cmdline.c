#include <pxs/protocol.h>
#include <lib/string.h>

static VOIDFRAMEX_CMDLINE_CONFIG config;

VOIDFRAMEX_CMDLINE_CONFIG* get_cmdline_config() {
    return &config;
}

void parse_cmdline(const char* cmdline) {
    if (find(cmdline, "verbose")) config.verbose = true;
}

///@section status functions

bool is_verbose() {
    return config.verbose;
}