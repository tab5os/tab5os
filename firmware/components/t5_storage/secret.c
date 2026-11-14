/* SPDX-License-Identifier: Apache-2.0
 * Sealed secrets (plan 9.4): see secret.h.
 */
#include <string.h>
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "t5_storage/secret.h"

#define MAGIC "T5S1"

/* SHA-256 over a label and the chip's IDs. A distinct label per use, so the
 * sealing key and the MAC key are unrelated. */
static t5_err_t derive(const char *label, uint8_t k[32])
{
    uint8_t material[64];
    size_t n = strlen(label);
    if (n > sizeof material - 6 - 16) return T5_ERR_INVALID_ARG;
    memcpy(material, label, n);
    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);
    memcpy(material + n, mac, 6);
    n += 6;
    uint8_t uid[16] = { 0 };
    esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, uid, 128);
    memcpy(material + n, uid, 16);
    n += 16;
    size_t kl = 0;
    psa_crypto_init();
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, material, n, k, 32, &kl);
    memset(material, 0, sizeof material);
    return st == PSA_SUCCESS && kl == 32 ? T5_OK : T5_ERR_FAIL;
}

static t5_err_t key(psa_key_id_t *out, psa_key_usage_t usage)
{
    uint8_t k[32];
    T5_RETURN_ON_ERR(derive("tab5os credentials v1", k));
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&a, usage);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 256);
    psa_status_t st = psa_import_key(&a, k, sizeof k, out);
    memset(k, 0, sizeof k);
    psa_reset_key_attributes(&a);
    return st == PSA_SUCCESS ? T5_OK : T5_ERR_FAIL;
}

int t5_secret_is_sealed(const void *in, size_t len)
{
    return len >= T5_SECRET_OVERHEAD && !memcmp(in, MAGIC, 4);
}

t5_err_t t5_secret_seal(const void *in, size_t len, void *out, size_t outcap, size_t *outlen)
{
    if (outcap < len + T5_SECRET_OVERHEAD) return T5_ERR_NO_MEM;
    psa_key_id_t k;
    T5_RETURN_ON_ERR(key(&k, PSA_KEY_USAGE_ENCRYPT));
    uint8_t *o = out;
    memcpy(o, MAGIC, 4);
    esp_fill_random(o + 4, 12);
    size_t n = 0;
    psa_status_t st = psa_aead_encrypt(k, PSA_ALG_GCM, o + 4, 12, (const uint8_t *)MAGIC, 4, in, len, o + 16,
                                       outcap - 16, &n);
    psa_destroy_key(k);
    if (st != PSA_SUCCESS) return T5_ERR_FAIL;
    *outlen = 16 + n;
    return T5_OK;
}

t5_err_t t5_secret_open(const void *in, size_t len, void *out, size_t outcap, size_t *outlen)
{
    if (!t5_secret_is_sealed(in, len)) return T5_ERR_INVALID_ARG;
    psa_key_id_t k;
    T5_RETURN_ON_ERR(key(&k, PSA_KEY_USAGE_DECRYPT));
    const uint8_t *i = in;
    size_t n = 0;
    psa_status_t st = psa_aead_decrypt(k, PSA_ALG_GCM, i + 4, 12, (const uint8_t *)MAGIC, 4, i + 16, len - 16, out,
                                       outcap, &n);
    psa_destroy_key(k);
    if (st != PSA_SUCCESS) return T5_ERR_INVALID_ARG;
    *outlen = n;
    return T5_OK;
}

t5_err_t t5_secret_mac(const void *msg, size_t len, uint8_t out[T5_SECRET_MAC_LEN])
{
    if (!msg || !out) return T5_ERR_INVALID_ARG;
    uint8_t k[32];
    T5_RETURN_ON_ERR(derive("tab5os device binding v1", k));
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&a, 256);
    psa_key_id_t id;
    psa_status_t st = psa_import_key(&a, k, sizeof k, &id);
    memset(k, 0, sizeof k);
    psa_reset_key_attributes(&a);
    if (st != PSA_SUCCESS) return T5_ERR_FAIL;
    size_t n = 0;
    st = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, len, out, T5_SECRET_MAC_LEN, &n);
    psa_destroy_key(id);
    return st == PSA_SUCCESS && n == T5_SECRET_MAC_LEN ? T5_OK : T5_ERR_FAIL;
}
