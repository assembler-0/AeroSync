#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hmac.h"

struct fkx_signature_footer {
    uint8_t signature[64];
    uint32_t magic; // 'SIG!'
};

#define SIG_MAGIC 0x21474953 // 'SIG!'

void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <cmd> <args>\n", prog);
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  genkey <key_file>\n");
    fprintf(stderr, "  sign <elf_file> <key_file>\n");
    fprintf(stderr, "  verify <elf_file> <key_file>\n");
}

int genkey(const char *key_file) {
    uint8_t key[64];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) { perror("fopen /dev/urandom"); return 1; }
    fread(key, 1, 64, f);
    fclose(f);

    f = fopen(key_file, "wb");
    if (!f) { perror(key_file); return 1; }
    fwrite(key, 1, 64, f);
    fclose(f);

    printf("Generated key: %s\n", key_file);
    return 0;
}

int sign(const char *elf_file, const char *key_file) {
    uint8_t key[64];
    FILE *f = fopen(key_file, "rb");
    if (!f) { perror(key_file); return 1; }
    fread(key, 1, 64, f);
    fclose(f);

    f = fopen(elf_file, "rb+");
    if (!f) { perror(elf_file); return 1; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc(size);
    fread(data, 1, size, f);

    uint8_t mac[64];
    hmac_sha512(key, 64, data, size, mac);

    struct fkx_signature_footer footer;
    memcpy(footer.signature, mac, 64);
    footer.magic = SIG_MAGIC;

    fseek(f, 0, SEEK_END);
    fwrite(&footer, sizeof(footer), 1, f);
    fclose(f);
    free(data);

    printf("Signed %s successfully (HMAC-SHA512)\n", elf_file);
    return 0;
}

int verify(const char *elf_file, const char *key_file) {
    uint8_t key[64];
    FILE *f = fopen(key_file, "rb");
    if (!f) { perror(key_file); return 1; }
    fread(key, 1, 64, f);
    fclose(f);

    f = fopen(elf_file, "rb");
    if (!f) { perror(elf_file); return 1; }

    fseek(f, 0, SEEK_END);
    long full_size = ftell(f);
    if (full_size < sizeof(struct fkx_signature_footer)) {
        fprintf(stderr, "File too small to be signed\n");
        fclose(f);
        return 1;
    }

    long data_size = full_size - sizeof(struct fkx_signature_footer);
    fseek(f, data_size, SEEK_SET);

    struct fkx_signature_footer footer;
    fread(&footer, sizeof(footer), 1, f);

    if (footer.magic != SIG_MAGIC) {
        fprintf(stderr, "No signature magic found\n");
        fclose(f);
        return 1;
    }

    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(data_size);
    fread(data, 1, data_size, f);
    fclose(f);

    uint8_t mac[64];
    hmac_sha512(key, 64, data, data_size, mac);

    if (memcmp(footer.signature, mac, 64) == 0) {
        printf("Signature VALID\n");
        free(data);
        return 0;
    } else {
        printf("Signature INVALID\n");
        free(data);
        return 1;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "genkey") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return genkey(argv[2]);
    } else if (strcmp(argv[1], "sign") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        return sign(argv[2], argv[3]);
    } else if (strcmp(argv[1], "verify") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        return verify(argv[2], argv[3]);
    } else {
        usage(argv[0]);
        return 1;
    }
}