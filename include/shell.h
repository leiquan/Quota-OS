#pragma once
enum shell_action { SHELL_PRINT, SHELL_CLEAR, SHELL_REBOOT, SHELL_OFF };
enum shell_action shell_execute(const char *command, char *output, unsigned capacity);
void shell_serial(void);
int shell_power(int reboot);
