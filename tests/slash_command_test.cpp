#include <cstdio>
#include <string>

#include "app_commands.h"

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("%s: %s\n", cond ? "  ok" : "  FAIL", what);
    if (!cond)
        failures++;
}

int main() {
    check(isSlashCommand("/new"), "/new is a command");
    check(isSlashCommand("/skills"), "/skills is a command");
    check(isSlashCommand("/scaffold build a game"), "/scaffold is a command");
    check(isSlashCommand("/skill:verify-ui check it"), "/skill:<name> is a command");
    check(isSlashCommand("/image /tmp/a.png"), "/image is a command");

    // The regression: a prompt that merely starts with a pasted absolute path
    // is plain text. Treated as a command, a Scaffold-mode prompt silently ran
    // through the Simple agent and no ledger was ever written.
    check(!isSlashCommand("/var/home/me/Screenshots/Screenshot_20261031.jpeg"),
          "bare image path is not a command");
    check(!isSlashCommand("/var/home/me/Screenshots/a.jpeg\nfix the orc mage"),
          "path then task text is not a command");
    check(!isSlashCommand("/tmp/f.txt read this"), "path + args is not a command");
    check(!isSlashCommand("/"), "a lone slash is not a command");
    check(!isSlashCommand(""), "empty prompt is not a command");
    check(!isSlashCommand("fix the bloom"), "plain prompt is not a command");
    check(!isSlashCommand("/newest"), "/newest is not /new");
    check(isSlashCommand("/skill:"), "/skill: with no name stays a command (error, not a task)");

    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
