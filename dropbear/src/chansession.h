/*
 * Dropbear - a SSH2 server
 * 
 * Copyright (c) 2002,2003 Matt Johnston
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */

#ifndef DROPBEAR_CHANSESSION_H_
#define DROPBEAR_CHANSESSION_H_

#include "loginrec.h"
#include "channel.h"
#include "listener.h"
#ifdef __ORBIS__
#include <pthread.h>
#include "ps4-shell.h"

enum ps4_command_state {
	PS4_BACKEND_IDLE,
	PS4_BACKEND_RUNNING,
	PS4_BACKEND_EXEC_READY,
	PS4_BACKEND_DONE
};
#endif

struct exitinfo {

	int exitpid; /* -1 if not exited */
	int exitstatus;
	int exitsignal;
	int exitcore;
};

struct ChanSess {

	char * cmd; /* command to exec */
	pid_t pid; /* child process pid */
	/* command that was sent by the client, if authorized_keys command= or
	dropbear -c was used */
	char *original_command;

	/* pty details */
	int master; /* the master terminal fd*/
	int slave;
	char * tty;
	char * term;

	/* exit details */
	struct exitinfo exit;

#ifdef __ORBIS__
	int ps4_backend_state;
	pthread_t ps4_thread;
	unsigned char ps4_thread_started;
	int ps4_backend_fd;
	int ps4_cancel_fd;
	unsigned char ps4_pty_requested;
	unsigned int ps4_term_cols;
	unsigned int ps4_term_rows;
	unsigned int ps4_term_width;
	unsigned int ps4_term_height;
	struct ps4_shell_state ps4_shell;
#endif


	/* These are only set temporarily before forking */
	/* Used to set $SSH_CONNECTION in the child session.  */
	char *connection_string;
	/* Used to set $SSH_CLIENT in the child session. */
	char *client_string;
	
#if DROPBEAR_X11FWD
	struct Listener * x11listener;
	int x11port;
	char * x11authprot;
	char * x11authcookie;
	unsigned int x11screennum;
	unsigned char x11singleconn;
#endif

#if DROPBEAR_SVR_AGENTFWD
	struct Listener * agentlistener;
	char * agentfile;
	char * agentdir;
#endif
};

struct ChildPid {
	pid_t pid;
	struct ChanSess * chansess;
};


void addnewvar(const char* param, const char* var);

void cli_send_chansess_request(void);
void cli_tty_cleanup(void);
void cli_chansess_winchange(void);
#if DROPBEAR_CLI_NETCAT
void cli_send_netcat_request(void);
#endif

void svr_chansessinitialise(void);
void svr_chansess_checksignal(void);
extern const struct ChanType svrchansess;

struct SigMap {
	int signal;
	char* name;
};

extern const struct SigMap signames[];

#endif /* DROPBEAR_CHANSESSION_H_ */
