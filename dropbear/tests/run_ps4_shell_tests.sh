#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
shell_test=$(mktemp /tmp/test-ps4-shell-XXXXXX)
scp_test=$(mktemp /tmp/test-ps4-scp-XXXXXX)
runtime_test=$(mktemp /tmp/test-ps4-runtime-XXXXXX)
run_test=$(mktemp /tmp/test-ps4-shell-run-XXXXXX)
trap 'rm -f "$shell_test" "$scp_test" "$runtime_test" "$run_test"' EXIT INT TERM

cd "$root"
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc \
	tests/test_ps4_shell_core.c src/ps4-shell.c src/ps4-runtime.c -o "$shell_test"
"$shell_test"
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc \
	tests/test_ps4_scp.c src/ps4-scp.c src/ps4-shell.c src/ps4-runtime.c -pthread -o "$scp_test"
"$scp_test"
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc \
	tests/test_ps4_runtime.c src/ps4-runtime.c -o "$runtime_test"
"$runtime_test"
${HOSTCC:-cc} -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc \
	tests/test_ps4_shell_run.c src/ps4-shell.c src/ps4-runtime.c -o "$run_test"
"$run_test"
