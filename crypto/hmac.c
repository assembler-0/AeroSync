#include <crypto/hmac.h>
#include <crypto/sha/sha512.h>
#include <lib/string.h>

void hmac_sha512(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t *mac) {
    sha512_context ctx;
    uint8_t k_ipad[128];
    uint8_t k_opad[128];
    uint8_t tk[64];
    int i;

    if (key_len > 128) {
        sha512(key, key_len, tk);
        key = tk;
        key_len = 64;
    }

    memset(k_ipad, 0, sizeof(k_ipad));
    memset(k_opad, 0, sizeof(k_opad));
    memcpy(k_ipad, key, key_len);
    memcpy(k_opad, key, key_len);

    for (i = 0; i < 128; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5c;
    }

    sha512_init(&ctx);
    sha512_update(&ctx, k_ipad, 128);
    sha512_update(&ctx, data, data_len);
    sha512_final(&ctx, mac);

    sha512_init(&ctx);
    sha512_update(&ctx, k_opad, 128);
    sha512_update(&ctx, mac, 64);
    sha512_final(&ctx, mac);
}
