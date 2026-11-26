#pragma once
#include <stdint.h>
enum auth_state { AUTH_WAIT, AUTH_SETUP, AUTH_CONFIRM, AUTH_SAVING, AUTH_LOGIN, AUTH_OPEN, AUTH_ERROR };
enum auth_state auth_state(void);
const char *auth_submit(const char *password);
void auth_logout(void);
void auth_disk_loaded(const uint8_t data[512]);
void auth_disk_verified(const uint8_t data[512]);
int auth_disk_store(const uint8_t data[512]);
void auth_serial_login(void);
void readline_secret(char *buf, int cap);
