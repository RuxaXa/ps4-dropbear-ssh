#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ps4-shell.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Host-Test fuer das neue Built-in `run <elf> [args...]`.
 * Auf dem Host laeuft der Pfad ueber libc fork/execv; auf Orbis ueber die
 * direkten Kernel-Syscalls in ps4-runtime.c. */

struct capture {
	char data[4096];
	size_t len;
};

static int capture_write(void *ctx, const void *data, size_t len) {
	struct capture *cap = ctx;
	if (cap->len + len + 1 > sizeof(cap->data)) {
		return -1;
	}
	memcpy(cap->data + cap->len, data, len);
	cap->len += len;
	cap->data[cap->len] = '\0';
	return 0;
}

int main(void) {
	struct ps4_shell_state state;

	/* Whitelist */
	assert(ps4_shell_handles("run /bin/echo ok") == 1);
	assert(ps4_shell_handles("run") == 1); /* wird erst im Dispatch auf Argumente geprueft */
	assert(ps4_shell_handles("notacommand") == 0);

	ps4_shell_init(&state, "/tmp");

	/* Erfolgsfall: Statuszeile, Kind-Ausgabe geht an stdout (nicht in den Capture) */
	struct capture cap;
	cap.len = 0;
	assert(ps4_shell_execute(&state, "run /bin/echo hello", capture_write, &cap) == 0);
	assert(strcmp(cap.data, "run: /bin/echo exit=0\n") == 0);

	/* Argumente werden durchgereicht (shell schreibt nur die Statuszeile) */
	cap.len = 0;
	assert(ps4_shell_execute(&state, "run /bin/echo a b", capture_write, &cap) == 0);
	assert(strcmp(cap.data, "run: /bin/echo exit=0\n") == 0);

	/* Nicht ausfuehrbarer Pfad: execv scheitert -> Kind exit=127 */
	cap.len = 0;
	assert(ps4_shell_execute(&state, "run /nonexistent-binary-xyz", capture_write, &cap) == 0);
	assert(strcmp(cap.data, "run: /nonexistent-binary-xyz exit=127\n") == 0);

	/* Ohne Argument: keine Ausfuehrung, Fehlerpfad */
	assert(ps4_shell_execute(&state, "run", capture_write, &cap) == 127);

	/* Fehlende Datei: execv scheitert im Kind -> exit=127 (resolve prueft keine Existenz) */
	cap.len = 0;
	assert(ps4_shell_execute(&state, "run ./definitely-missing", capture_write, &cap) == 0);
	assert(strcmp(cap.data, "run: ./definitely-missing exit=127\n") == 0);

	printf("test_ps4_shell_run: PASS\n");
	return 0;
}
