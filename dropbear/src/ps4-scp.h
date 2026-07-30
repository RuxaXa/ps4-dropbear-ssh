#ifndef DROPBEAR_PS4_SCP_H_
#define DROPBEAR_PS4_SCP_H_

#include "ps4-shell.h"

#define PS4_SCP_PATH_MAX PS4_SHELL_PATH_MAX
#define PS4_SCP_MAX_FILE_SIZE (8ULL * 1024ULL * 1024ULL * 1024ULL)
#define PS4_SCP_SAFE_SEND_MAX 8192ULL

enum ps4_scp_mode {
	PS4_SCP_RECEIVE,
	PS4_SCP_SEND
};

struct ps4_scp_request {
	enum ps4_scp_mode mode;
	char path[PS4_SCP_PATH_MAX];
	int preserve;
	int target_should_be_directory;
};

int ps4_scp_parse_command(const char *command, const char *cwd,
		struct ps4_scp_request *request);
int ps4_scp_serve(int fd, const struct ps4_scp_request *request);

#endif
