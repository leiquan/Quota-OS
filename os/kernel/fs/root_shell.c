#include "auth.h"
#include "quotaos.h"
#include "root_service.h"
#include "root_shell.h"

static enum root_operation pending_op;
static int pending, discard;
static char output[ROOT_SERVICE_BYTES + 1];

const char *root_shell_command(const char *command)
{
    if (auth_state() != AUTH_OPEN) return "Login required.";
    if (!strcmp(command, "pwd")) return "/";
    unsigned key = 0;
    while (command[key] && command[key] != ' ') ++key;
    if (key >= 6) return NULL;
    char verb[6];
    memcpy(verb, command, key);
    verb[key] = 0;
    enum root_operation op;
    if (!strcmp(verb, "ls")) op = ROOT_LIST;
    else if (!strcmp(verb, "cat")) op = ROOT_READ;
    else if (!strcmp(verb, "write")) op = ROOT_SAVE;
    else if (!strcmp(verb, "rm")) op = ROOT_REMOVE;
    else return NULL;

    const char *arg = command + key;
    while (*arg == ' ') ++arg;
    if (op == ROOT_LIST && *arg && strcmp(arg, "/")) return "Usage: ls [/]";
    if (*arg == '/') ++arg;
    char file[256];
    unsigned n = 0;
    while (arg[n] && arg[n] != ' ') {
        if (n + 1 >= sizeof file || arg[n] == '/') return "Invalid root file name.";
        file[n] = arg[n]; ++n;
    }
    file[n] = 0;
    const char *value = arg + n;
    while (*value == ' ') ++value;
    if (op != ROOT_LIST && (!n || (op == ROOT_SAVE ? !*value : !!*value)))
        return op == ROOT_SAVE ? "Usage: write FILE TEXT" : op == ROOT_READ ? "Usage: cat FILE" : "Usage: rm FILE";
    if (pending) return "Root disk busy.";
    unsigned bytes = 0;
    while (value[bytes]) ++bytes;
    int rc = root_service_submit(op, op == ROOT_LIST ? NULL : file, value, bytes);
    if (rc) return rc > 0 ? "Root disk busy." : "Unable to queue root request.";
    pending = 1;
    pending_op = op;
    return "Root request queued.";
}

void root_shell_logout(void) { discard = pending; }

const char *root_shell_poll(void)
{
    if (!pending) return NULL;
    size_t bytes;
    int result;
    if (root_service_take(output, ROOT_SERVICE_BYTES, &bytes, &result) != 1) return NULL;
    pending = 0;
    if (discard || auth_state() != AUTH_OPEN) { discard = 0; return NULL; }
    if (result) return ext4_result_text(result);
    if (pending_op == ROOT_SAVE) return "Saved.";
    if (pending_op == ROOT_REMOVE) return "Removed.";
    if (!bytes) return pending_op == ROOT_LIST ? "No files." : "Empty file.";
    output[bytes] = 0;
    return output;
}
