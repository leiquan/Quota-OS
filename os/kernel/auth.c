#include "quotaos.h"
#include "auth.h"
#include "../third_party/monocypher/monocypher.h"

/* Sector 63: tag (8), installation salt (16), verifier (32), zero padding,
 * then a BLAKE2b checksum (32). Only the installer creates a fresh tag. */
static uint8_t record[512], first[32];
static uint64_t work[128 * 1024 / sizeof(uint64_t)];
static enum auth_state state;
static uint64_t retry_us;

static int bytes_equal(const void *left, const void *right, unsigned n)
{
    const uint8_t *a = left, *b = right;
    for (unsigned i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

enum auth_state auth_state(void) { return state; }
void auth_disk_loaded(const uint8_t data[512])
{
    uint8_t sum[32];
    crypto_blake2b(sum, 32, data, 480);
    state = AUTH_ERROR;
    if (crypto_verify32(sum, data + 480)) return;
    memcpy(record, data, sizeof record);
    if (bytes_equal(record, "QAINIT01", 8)) state = AUTH_SETUP;
    if (bytes_equal(record, "QAACT001", 8)) state = AUTH_LOGIN;
}
void auth_disk_verified(const uint8_t data[512])
{
    state = bytes_equal(record, data, sizeof record) ? AUTH_LOGIN : AUTH_ERROR;
}
const char *auth_submit(const char *pass)
{
    size_t n = 0;
    while (n <= 64 && pass[n]) ++n;
    if (state != AUTH_SETUP && state != AUTH_CONFIRM && state != AUTH_LOGIN)
        return "Account unavailable.";
    if (now_us() < retry_us) return "Please wait before retrying.";
    if (!n || n > 64) return "Enter 1 to 64 password bytes.";
    uint8_t result[32];
    crypto_argon2(result, 32, work, (crypto_argon2_config){CRYPTO_ARGON2_ID,128,3,1},
                  (crypto_argon2_inputs){(const uint8_t *)pass, record+8, n, 16},
                  crypto_argon2_no_extras);
    const char *message = "";
    if (state == AUTH_SETUP) {
        memcpy(first, result, 32);
        state = AUTH_CONFIRM;
    } else if (state == AUTH_CONFIRM) {
        if (crypto_verify32(first, result)) {
            state = AUTH_SETUP;
            message = "Passwords do not match.";
        } else {
            memcpy(record, "QAACT001", 8);
            memcpy(record+24, result, 32);
            crypto_blake2b(record+480, 32, record, 480);
            state = auth_disk_store(record) ? AUTH_ERROR : AUTH_SAVING;
        }
        crypto_wipe(first, sizeof first);
    } else if (!crypto_verify32(record+24, result)) {
        state = AUTH_OPEN;
    } else {
        retry_us = now_us() + 250000;
        message = "Incorrect password.";
    }
    crypto_wipe(result, sizeof result);
    return message;
}
void auth_logout(void) { if (state == AUTH_OPEN) state = AUTH_LOGIN; }
void auth_serial_login(void)
{
    char pass[65];
    while (state != AUTH_OPEN) {
        note_disk_poll();
        if (state == AUTH_WAIT || state == AUTH_SAVING || state == AUTH_ERROR) {
            if (state == AUTH_ERROR || !strcmp(note_disk_status(),"Disk error") ||
                !strcmp(note_disk_status(),"No Notes disk")) {
                printf("Account storage unavailable. Login closed.\n");
                for (;;) asm volatile("wfi");
            }
            asm volatile("wfi");
            continue;
        }
        printf("admin | %s: ", state == AUTH_SETUP ? "Set password" :
               state == AUTH_CONFIRM ? "Confirm password" : "Password");
        readline_secret(pass, sizeof pass);
        const char *msg = auth_submit(pass);
        crypto_wipe(pass, sizeof pass);
        if (*msg) printf("%s\n",msg);
    }
}
