#include <string.h>
#include <crypto.h>


char *key;
int key_len;

int
crypto_init(const char *password) {
    key = strdup(password);
    key_len = strlen(key);

    return 0;
}




void xor_crypt(const uint8_t *input, uint8_t *output, const int len)
{
    int i;
    for (i=0; i< len; i++) {
        output[i] = input[i] ^ key[i % key_len];
    }
}



int
crypto_encrypt(uint8_t *c, const uint8_t *p, const uint32_t plen) {
    xor_crypt(p, c, plen);

    return 0;
}




int
crypto_decrypt(uint8_t *p, const uint8_t *c, const uint32_t clen) {
    xor_crypt(c, p, clen);

    return 0;
}
