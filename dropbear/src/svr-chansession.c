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

#include "includes.h"
#include "packet.h"
#include "buffer.h"
#include "session.h"
#include "dbutil.h"
#include "channel.h"
#include "chansession.h"
#include "sshpty.h"
#include "termcodes.h"
#include "ssh.h"
#include "dbrandom.h"
#include "x11fwd.h"
#include "agentfwd.h"
#include "runopts.h"
#include "auth.h"
#ifdef __ORBIS__
#include "ps4-runtime.h"
#include "ps4-scp.h"
#include <pthread.h>

struct ps4_system_version {
	unsigned long unknown1;
	char version[0x1c];
	unsigned int binary_version;
	unsigned long unknown2;
};

void sceKernelGetSystemSwVersion(struct ps4_system_version *version);

struct ps4_shell_context {
	int fd;
	struct ChanSess *chansess;
};

struct ps4_scp_context {
	int fd;
	struct ChanSess *chansess;
	struct ps4_scp_request request;
};

struct ps4_capture_context {
	char *output;
	size_t size;
	size_t used;
};

#endif

/* Handles sessions (either shells or programs) requested by the client */

static int sessioncommand(struct Channel *channel, struct ChanSess *chansess,
		int iscmd, int issubsys);
static int sessionpty(struct ChanSess * chansess);
static int sessionsignal(const struct ChanSess *chansess);
static int noptycommand(struct Channel *channel, struct ChanSess *chansess);
static int ptycommand(struct Channel *channel, struct ChanSess *chansess);
static int sessionwinchange(struct ChanSess *chansess);
static void execchild(const void *user_data_chansess);
static void addchildpid(struct ChanSess *chansess, pid_t pid);
static void sesssigchild_handler(int val);
static void closechansess(const struct Channel *channel);
static void cleanupchansess(const struct Channel *channel);
static int newchansess(struct Channel *channel);
static void chansessionrequest(struct Channel *channel);
static int sesscheckclose(struct Channel *channel);
#ifdef __ORBIS__
static int ps4_builtin_command(struct Channel *channel, struct ChanSess *chansess);
static int ps4_shell_start(struct Channel *channel, struct ChanSess *chansess);
static int ps4_scp_start(struct Channel *channel, struct ChanSess *chansess);
static int ps4_format_command(struct ChanSess *chansess, const char *cmd,
		char *output, size_t output_size, int *output_len);
static int ps4_resolve_path(const char *cwd, const char *path,
		char *resolved, size_t resolved_size);
static int ps4_list_directory(const char *path, int show_hidden,
		char *output, size_t output_size, int *output_len);
static void ps4_write_prompt(int fd, const struct ChanSess *chansess);
static int ps4_write_all(int fd, const char *data, size_t len);
static int ps4_write_pipe_all(int fd, const char *data, size_t len);
static int ps4_write_terminal_output(int fd, const char *data, size_t len);
static int ps4_terminal_shell_output(void *ctx, const void *data, size_t len);
static int ps4_capture_shell_output(void *ctx, const void *data, size_t len);
static void *ps4_shell_thread(void *arg);
static void *ps4_scp_thread(void *arg);
#endif

static void send_exitsignalstatus(const struct Channel *channel);
static void send_msg_chansess_exitstatus(const struct Channel * channel,
		const struct ChanSess * chansess);
static void send_msg_chansess_exitsignal(const struct Channel * channel,
		const struct ChanSess * chansess);
static void get_termmodes(const struct ChanSess *chansess);

const struct ChanType svrchansess = {
	"session", /* name */
	newchansess, /* inithandler */
	sesscheckclose, /* checkclosehandler */
	chansessionrequest, /* reqhandler */
	closechansess, /* closehandler */
	cleanupchansess /* cleanup */
};

/* Returns whether the channel is ready to close. The child process
   must not be running (has never started, or has exited) */
static int sesscheckclose(struct Channel *channel) {
	struct ChanSess *chansess = (struct ChanSess*)channel->typedata;
#ifdef __ORBIS__
	int backend_state = __atomic_load_n(&chansess->ps4_backend_state, __ATOMIC_ACQUIRE);
	TRACE(("sesscheckclose, ps4 backend state %d", backend_state))
	if (backend_state == PS4_BACKEND_RUNNING && channel->recv_close) {
		int backend_fd = __atomic_exchange_n(&chansess->ps4_backend_fd, -1,
				__ATOMIC_ACQ_REL);
		if (backend_fd >= 0) {
			__atomic_store_n(&chansess->ps4_cancel_fd, backend_fd, __ATOMIC_RELEASE);
			(void)shutdown(backend_fd, SHUT_RDWR);
		}
	}
	if (backend_state == PS4_BACKEND_DONE) {
		channel->flushing = 1;
	}
	return backend_state == PS4_BACKEND_IDLE || backend_state == PS4_BACKEND_DONE;
#else
	int exitpid = chansess->exit.exitpid;
	TRACE(("sesscheckclose, pid %d, exitpid %d", chansess->pid, exitpid))

	if (exitpid != -1) {
		channel->flushing = 1;
	}
	return chansess->pid == 0 || exitpid != -1;
#endif
}

/* Handler for childs exiting, store the state for return to the client */

/* There's a particular race we have to watch out for: if the forked child
 * executes, exits, and this signal-handler is called, all before the parent
 * gets to run, then the childpids[] array won't have the pid in it. Hence we
 * use the svr_ses.lastexit struct to hold the exit, which is then compared by
 * the parent when it runs. This work correctly at least in the case of a
 * single shell spawned (ie the usual case) */
void svr_chansess_checksignal(void) {
	int status;
	pid_t pid;

	if (!ses.channel_signal_pending) {
		return;
	}

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		unsigned int i;
		struct exitinfo *ex = NULL;
		TRACE(("svr_chansess_checksignal : pid %d", pid))

		ex = NULL;
		/* find the corresponding chansess */
		for (i = 0; i < svr_ses.childpidsize; i++) {
			if (svr_ses.childpids[i].pid == pid) {
				TRACE(("found match session"));
				ex = &svr_ses.childpids[i].chansess->exit;
				break;
			}
		}

		/* If the pid wasn't matched, then we might have hit the race mentioned
		 * above. So we just store the info for the parent to deal with */
		if (ex == NULL) {
			TRACE(("using lastexit"));
			ex = &svr_ses.lastexit;
		}

		ex->exitpid = pid;
		if (WIFEXITED(status)) {
			ex->exitstatus = WEXITSTATUS(status);
		}
		if (WIFSIGNALED(status)) {
			ex->exitsignal = WTERMSIG(status);
#if !defined(AIX) && defined(WCOREDUMP)
			ex->exitcore = WCOREDUMP(status);
#else
			ex->exitcore = 0;
#endif
		} else {
			/* we use this to determine how pid exited */
			ex->exitsignal = -1;
		}
	}
}

static void sesssigchild_handler(int UNUSED(dummy)) {
	struct sigaction sa_chld;

	const int saved_errno = errno;

	/* Make sure that the main select() loop wakes up */
	while (1) {
		/* isserver is just a random byte to write. We can't do anything
		about an error so should just ignore it */
		if (write(ses.signal_pipe[1], &ses.isserver, 1) == 1
				|| errno != EINTR) {
			break;
		}
	}

	sa_chld.sa_handler = sesssigchild_handler;
	sa_chld.sa_flags = SA_NOCLDSTOP;
	sigemptyset(&sa_chld.sa_mask);
	sigaction(SIGCHLD, &sa_chld, NULL);

	errno = saved_errno;
}

/* send the exit status or the signal causing termination for a session */
static void send_exitsignalstatus(const struct Channel *channel) {

	struct ChanSess *chansess = (struct ChanSess*)channel->typedata;

	if (chansess->exit.exitpid >= 0) {
		if (chansess->exit.exitsignal > 0) {
			send_msg_chansess_exitsignal(channel, chansess);
		} else {
			send_msg_chansess_exitstatus(channel, chansess);
		}
	}
}

/* send the exitstatus to the client */
static void send_msg_chansess_exitstatus(const struct Channel * channel,
		const struct ChanSess * chansess) {

	dropbear_assert(chansess->exit.exitpid != -1);
	dropbear_assert(chansess->exit.exitsignal == -1);

	CHECKCLEARTOWRITE();

	buf_putbyte(ses.writepayload, SSH_MSG_CHANNEL_REQUEST);
	buf_putint(ses.writepayload, channel->remotechan);
	buf_putstring(ses.writepayload, "exit-status", 11);
	buf_putbyte(ses.writepayload, 0); /* boolean FALSE */
	buf_putint(ses.writepayload, chansess->exit.exitstatus);

	encrypt_packet();

}

/* send the signal causing the exit to the client */
static void send_msg_chansess_exitsignal(const struct Channel * channel,
		const struct ChanSess * chansess) {

	int i;
	char* signame = NULL;
	dropbear_assert(chansess->exit.exitpid != -1);
	dropbear_assert(chansess->exit.exitsignal > 0);

	TRACE(("send_msg_chansess_exitsignal %d", chansess->exit.exitsignal))

	CHECKCLEARTOWRITE();

	/* we check that we can match a signal name, otherwise
	 * don't send anything */
	for (i = 0; signames[i].name != NULL; i++) {
		if (signames[i].signal == chansess->exit.exitsignal) {
			signame = signames[i].name;
			break;
		}
	}

	if (signame == NULL) {
		return;
	}

	buf_putbyte(ses.writepayload, SSH_MSG_CHANNEL_REQUEST);
	buf_putint(ses.writepayload, channel->remotechan);
	buf_putstring(ses.writepayload, "exit-signal", 11);
	buf_putbyte(ses.writepayload, 0); /* boolean FALSE */
	buf_putstring(ses.writepayload, signame, strlen(signame));
	buf_putbyte(ses.writepayload, chansess->exit.exitcore);
	buf_putstring(ses.writepayload, "", 0); /* error msg */
	buf_putstring(ses.writepayload, "", 0); /* lang */

	encrypt_packet();
}

/* set up a session channel */
static int newchansess(struct Channel *channel) {

	struct ChanSess *chansess;

	TRACE(("new chansess %p", (void*)channel))

	dropbear_assert(channel->typedata == NULL);

	chansess = (struct ChanSess*)m_malloc(sizeof(struct ChanSess));
	chansess->cmd = NULL;
	chansess->connection_string = NULL;
	chansess->client_string = NULL;
	chansess->pid = 0;

	/* pty details */
	chansess->master = -1;
	chansess->slave = -1;
	chansess->tty = NULL;
	chansess->term = NULL;

	chansess->exit.exitpid = -1;
#ifdef __ORBIS__
	chansess->ps4_backend_state = PS4_BACKEND_IDLE;
	chansess->ps4_thread_started = 0;
	chansess->ps4_backend_fd = -1;
	chansess->ps4_cancel_fd = -1;
	chansess->ps4_pty_requested = 0;
	chansess->ps4_term_cols = 0;
	chansess->ps4_term_rows = 0;
	chansess->ps4_term_width = 0;
	chansess->ps4_term_height = 0;
	ps4_shell_init(&chansess->ps4_shell, "/data/dropbear");
#endif

	channel->typedata = chansess;

#if DROPBEAR_X11FWD
	chansess->x11listener = NULL;
	chansess->x11authprot = NULL;
	chansess->x11authcookie = NULL;
#endif

#if DROPBEAR_SVR_AGENTFWD
	chansess->agentlistener = NULL;
	chansess->agentfile = NULL;
	chansess->agentdir = NULL;
#endif

	/* Will drop to DROPBEAR_PRIO_NORMAL if a non-tty command starts */
	channel->prio = DROPBEAR_PRIO_LOWDELAY;

	return 0;

}

static struct logininfo* 
chansess_login_alloc(const struct ChanSess *chansess) {
	struct logininfo * li;
	li = login_alloc_entry(chansess->pid, ses.authstate.username,
			svr_ses.remotehost, chansess->tty);
	return li;
}

/* send exit status message before the channel is closed */
static void closechansess(const struct Channel *channel) {
	struct ChanSess *chansess;

	TRACE(("enter closechansess"))

	chansess = (struct ChanSess*)channel->typedata;

	if (chansess == NULL) {
		TRACE(("leave closechansess: chansess == NULL"))
		return;
	}

	send_exitsignalstatus(channel);
	TRACE(("leave closechansess"))
}

/* clean a session channel */
static void cleanupchansess(const struct Channel *channel) {

	struct ChanSess *chansess;
	unsigned int i;
	struct logininfo *li;

	TRACE(("enter closechansess"))

	chansess = (struct ChanSess*)channel->typedata;

	if (chansess == NULL) {
		TRACE(("leave closechansess: chansess == NULL"))
		return;
	}

#ifdef __ORBIS__
	if (chansess->ps4_thread_started) {
		int backend_fd = __atomic_exchange_n(&chansess->ps4_backend_fd, -1,
				__ATOMIC_ACQ_REL);
		int cancel_fd = __atomic_exchange_n(&chansess->ps4_cancel_fd, -1,
				__ATOMIC_ACQ_REL);
		if (backend_fd >= 0) {
			(void)shutdown(backend_fd, SHUT_RDWR);
		}
		if (cancel_fd >= 0) {
			(void)shutdown(cancel_fd, SHUT_RDWR);
		}
		(void)pthread_join(chansess->ps4_thread, NULL);
		if (backend_fd >= 0) {
			m_close(backend_fd);
		}
		if (cancel_fd >= 0) {
			m_close(cancel_fd);
		}
		chansess->ps4_thread_started = 0;
	}
#endif

	m_free(chansess->cmd);
	m_free(chansess->term);
	m_free(chansess->original_command);

	if (chansess->tty) {
		/* write the utmp/wtmp login record */
		li = chansess_login_alloc(chansess);

		svr_raise_gid_utmp();
		login_logout(li);
		svr_restore_gid();

		login_free_entry(li);

		pty_release(chansess->tty);
		m_free(chansess->tty);
	}

#if DROPBEAR_X11FWD
	x11cleanup(chansess);
#endif

#if DROPBEAR_SVR_AGENTFWD
	svr_agentcleanup(chansess);
#endif

	/* clear child pid entries */
	for (i = 0; i < svr_ses.childpidsize; i++) {
		if (svr_ses.childpids[i].chansess == chansess) {
			dropbear_assert(svr_ses.childpids[i].pid > 0);
			TRACE(("closing pid %d", svr_ses.childpids[i].pid))
			TRACE(("exitpid is %d", chansess->exit.exitpid))
			svr_ses.childpids[i].pid = -1;
			svr_ses.childpids[i].chansess = NULL;
		}
	}
				
	m_free(chansess);

	TRACE(("leave closechansess"))
}

/* Handle requests for a channel. These can be execution requests,
 * or x11/authagent forwarding. These are passed to appropriate handlers */
static void chansessionrequest(struct Channel *channel) {

	char * type = NULL;
	unsigned int typelen;
	unsigned char wantreply;
	int ret = 1;
	struct ChanSess *chansess;

	TRACE(("enter chansessionrequest"))

	type = buf_getstring(ses.payload, &typelen);
	wantreply = buf_getbool(ses.payload);

	if (typelen > MAX_NAME_LEN) {
		TRACE(("leave chansessionrequest: type too long")) /* XXX send error?*/
		goto out;
	}

	chansess = (struct ChanSess*)channel->typedata;
	dropbear_assert(chansess != NULL);
	TRACE(("type is %s", type))

	if (strcmp(type, "window-change") == 0) {
		ret = sessionwinchange(chansess);
	} else if (strcmp(type, "shell") == 0) {
		ret = sessioncommand(channel, chansess, 0, 0);
	} else if (strcmp(type, "pty-req") == 0) {
		ret = sessionpty(chansess);
	} else if (strcmp(type, "exec") == 0) {
		ret = sessioncommand(channel, chansess, 1, 0);
	} else if (strcmp(type, "subsystem") == 0) {
		ret = sessioncommand(channel, chansess, 1, 1);
#if DROPBEAR_X11FWD
	} else if (strcmp(type, "x11-req") == 0) {
		ret = x11req(chansess);
#endif
#if DROPBEAR_SVR_AGENTFWD
	} else if (strcmp(type, "auth-agent-req@openssh.com") == 0) {
		ret = svr_agentreq(chansess);
#endif
	} else if (strcmp(type, "signal") == 0) {
		ret = sessionsignal(chansess);
	} else {
		/* etc, todo "env", "subsystem" */
	}

out:

	if (wantreply) {
		if (ret == DROPBEAR_SUCCESS) {
			send_msg_channel_success(channel);
		} else {
			send_msg_channel_failure(channel);
		}
	}
#ifdef __ORBIS__
	if (ret == DROPBEAR_SUCCESS) {
		int expected = PS4_BACKEND_EXEC_READY;
		(void)__atomic_compare_exchange_n(&chansess->ps4_backend_state,
				&expected, PS4_BACKEND_DONE, 0,
				__ATOMIC_RELEASE, __ATOMIC_RELAXED);
	}
#endif

	m_free(type);
	TRACE(("leave chansessionrequest"))
}


/* Send a signal to a session's process as requested by the client*/
static int sessionsignal(const struct ChanSess *chansess) {
	TRACE(("sessionsignal"))

#ifdef __ORBIS__
	/* In-process PS4 backends have no child PID. Never route SSH signal
	 * requests to a synthetic value such as PID 1. */
	(void)chansess;
	return DROPBEAR_FAILURE;
#else
	int sig = 0;
	char* signame = NULL;
	int i;

	if (chansess->pid == 0) {
		TRACE(("sessionsignal: done no pid"))
		/* haven't got a process pid yet */
		return DROPBEAR_FAILURE;
	}

	if (svr_opts.forced_command || svr_pubkey_has_forced_command()) {
		TRACE(("disallowed signal for forced_command"));
		return DROPBEAR_FAILURE;
	}

	if (DROPBEAR_SVR_MULTIUSER && !DROPBEAR_SVR_DROP_PRIVS) {
		TRACE(("disallow signal without drop privs"));
		return DROPBEAR_FAILURE;
	}

	signame = buf_getstring(ses.payload, NULL);

	for (i = 0; signames[i].name != NULL; i++) {
		if (strcmp(signames[i].name, signame) == 0) {
			sig = signames[i].signal;
			break;
		}
	}

	m_free(signame);

	TRACE(("sessionsignal: pid %d signal %d", (int)chansess->pid, sig))
	if (sig == 0) {
		/* failed */
		return DROPBEAR_FAILURE;
	}
			
	if (kill(chansess->pid, sig) < 0) {
		TRACE(("sessionsignal: kill() errored"))
		return DROPBEAR_FAILURE;
	} 

	return DROPBEAR_SUCCESS;
#endif
}
/* Let the process know that the window size has changed, as notified from the
 * client. Returns DROPBEAR_SUCCESS or DROPBEAR_FAILURE */
static int sessionwinchange(struct ChanSess *chansess) {
#ifdef __ORBIS__
	chansess->ps4_term_cols = buf_getint(ses.payload);
	chansess->ps4_term_rows = buf_getint(ses.payload);
	chansess->ps4_term_width = buf_getint(ses.payload);
	chansess->ps4_term_height = buf_getint(ses.payload);
	return DROPBEAR_SUCCESS;
#else
	int termc, termr, termw, termh;

	if (chansess->master < 0) {
		/* haven't got a pty yet */
		return DROPBEAR_FAILURE;
	}
			
	termc = buf_getint(ses.payload);
	termr = buf_getint(ses.payload);
	termw = buf_getint(ses.payload);
	termh = buf_getint(ses.payload);
	
	pty_change_window_size(chansess->master, termr, termc, termw, termh);

	return DROPBEAR_SUCCESS;
#endif
}

static void get_termmodes(const struct ChanSess *chansess) {

	struct termios termio;
	unsigned char opcode;
	unsigned int value;
	const struct TermCode *termcode;
	unsigned int len;

	TRACE(("enter get_termmodes"))
	if (tcgetattr(chansess->master, &termio) == -1) {
		return;
	}

	len = buf_getint(ses.payload);
	TRACE(("term mode str %d p->l %d p->p %d",
				len, ses.payload->len, ses.payload->pos));
	if (len != ses.payload->len - ses.payload->pos) {
		dropbear_exit("Bad term mode string");
	}
	if (len == 0) {
		TRACE(("leave get_termmodes: empty terminal modes string"))
		return;
	}

	while (((opcode = buf_getbyte(ses.payload)) != 0x00) && opcode <= 159) {
		value = buf_getint(ses.payload);
		if (opcode > MAX_TERMCODE) {
			continue;
		}
		termcode = &termcodes[(unsigned int)opcode];
		switch (termcode->type) {
			case TERMCODE_NONE:
				break;
			case TERMCODE_CONTROLCHAR:
				termio.c_cc[termcode->mapcode] = value;
				break;
			case TERMCODE_INPUT:
				if (value) {
					termio.c_iflag |= termcode->mapcode;
				} else {
					termio.c_iflag &= ~(termcode->mapcode);
				}
				break;
			case TERMCODE_OUTPUT:
				if (value) {
					termio.c_oflag |= termcode->mapcode;
				} else {
					termio.c_oflag &= ~(termcode->mapcode);
				}
				break;
			case TERMCODE_LOCAL:
				if (value) {
					termio.c_lflag |= termcode->mapcode;
				} else {
					termio.c_lflag &= ~(termcode->mapcode);
				}
				break;
			case TERMCODE_CONTROL:
				if (value) {
					termio.c_cflag |= termcode->mapcode;
				} else {
					termio.c_cflag &= ~(termcode->mapcode);
				}
				break;
		}
	}
	if (tcsetattr(chansess->master, TCSANOW, &termio) < 0) {
		dropbear_log(LOG_INFO, "Error setting terminal attributes");
	}
	TRACE(("leave get_termmodes"))
}

/* Set up a session pty which will be used to execute the shell or program.
 * The pty is allocated now, and kept for when the shell/program executes.
 * Returns DROPBEAR_SUCCESS or DROPBEAR_FAILURE */
static int sessionpty(struct ChanSess *chansess) {
	unsigned int termlen;
#ifndef __ORBIS__
	char namebuf[65];
	struct passwd *pw = NULL;
#endif

	TRACE(("enter sessionpty"))
	if (!svr_pubkey_allows_pty()) {
		TRACE(("leave sessionpty : pty forbidden by public key option"))
		return DROPBEAR_FAILURE;
	}
	if (chansess->term != NULL) {
		TRACE(("leave sessionpty: multiple pty requests"))
		return DROPBEAR_FAILURE;
	}

	chansess->term = buf_getstring(ses.payload, &termlen);
	if (termlen > MAX_TERM_LEN) {
		TRACE(("leave sessionpty: term len too long"))
		m_free(chansess->term);
		chansess->term = NULL;
		return DROPBEAR_FAILURE;
	}

#ifdef __ORBIS__
	chansess->ps4_term_cols = buf_getint(ses.payload);
	chansess->ps4_term_rows = buf_getint(ses.payload);
	chansess->ps4_term_width = buf_getint(ses.payload);
	chansess->ps4_term_height = buf_getint(ses.payload);
	{
		char *modes = buf_getstring(ses.payload, NULL);
		m_free(modes);
	}
	chansess->ps4_pty_requested = 1;
	return DROPBEAR_SUCCESS;
#else
	if (chansess->master != -1) {
		dropbear_exit("Multiple pty requests");
	}
	if (pty_allocate(&chansess->master, &chansess->slave, namebuf, 64) == 0) {
		TRACE(("leave sessionpty: failed to allocate pty"))
		return DROPBEAR_FAILURE;
	}
	chansess->tty = m_strdup(namebuf);
	if (!chansess->tty) {
		dropbear_exit("Out of memory");
	}
	pw = getpwnam(ses.authstate.pw_name);
	if (!pw) {
		dropbear_exit("getpwnam failed after succeeding previously");
	}
	pty_setowner(pw, chansess->tty);
	sessionwinchange(chansess);
	get_termmodes(chansess);
	TRACE(("leave sessionpty"))
	return DROPBEAR_SUCCESS;
#endif
}

#if !DROPBEAR_VFORK
static void make_connection_string(struct ChanSess *chansess) {
	char *local_ip, *local_port, *remote_ip, *remote_port;
	size_t len;
	get_socket_address(ses.sock_in, &local_ip, &local_port, &remote_ip, &remote_port, 0);

	/* "remoteip remoteport localip localport" */
	len = strlen(local_ip) + strlen(remote_ip) + 20;
	chansess->connection_string = m_malloc(len);
	snprintf(chansess->connection_string, len, "%s %s %s %s", remote_ip, remote_port, local_ip, local_port);

	/* deprecated but bash only loads .bashrc if SSH_CLIENT is set */ 
	/* "remoteip remoteport localport" */
	len = strlen(remote_ip) + 20;
	chansess->client_string = m_malloc(len);
	snprintf(chansess->client_string, len, "%s %s %s", remote_ip, remote_port, local_port);

	m_free(local_ip);
	m_free(local_port);
	m_free(remote_ip);
	m_free(remote_port);
}
#endif

/* Handle a command request from the client. This is used for both shell
 * and command-execution requests, and passes the command to
 * noptycommand or ptycommand as appropriate.
 * Returns DROPBEAR_SUCCESS or DROPBEAR_FAILURE */
static int sessioncommand(struct Channel *channel, struct ChanSess *chansess,
		int iscmd, int issubsys) {

	unsigned int cmdlen = 0;
	int ret;

	TRACE(("enter sessioncommand %d", channel->index))

#ifdef __ORBIS__
	if (__atomic_load_n(&chansess->ps4_backend_state, __ATOMIC_ACQUIRE) != PS4_BACKEND_IDLE) {
#else
	if (chansess->pid != 0) {
#endif
		/* Note that only one command can _succeed_. The client might try
		 * one command (which fails), then try another. Ie fallback
		 * from sftp to scp */
		TRACE(("leave sessioncommand, already have a command"))
		return DROPBEAR_FAILURE;
	}

	if (iscmd) {
		/* "exec" */
		if (chansess->cmd == NULL) {
			chansess->cmd = buf_getstring(ses.payload, &cmdlen);

			if (cmdlen > MAX_CMD_LEN) {
				m_free(chansess->cmd);
				chansess->cmd = NULL;
				/* TODO - send error - too long ? */
				TRACE(("leave sessioncommand, command too long %d", cmdlen))
				return DROPBEAR_FAILURE;
			}
		}
		if (issubsys) {
#if DROPBEAR_SFTPSERVER
			if ((cmdlen == 4) && strncmp(chansess->cmd, "sftp", 4) == 0) {
				char *expand_path = expand_homedir_path(SFTPSERVER_PATH);
				m_free(chansess->cmd);
				chansess->cmd = m_strdup(expand_path);
				m_free(expand_path);
			} else 
#endif
			{
				m_free(chansess->cmd);
				chansess->cmd = NULL;
				TRACE(("leave sessioncommand, unknown subsystem"))
				return DROPBEAR_FAILURE;
			}
		}
	}
	

	/* take global command into account */
	if (svr_opts.forced_command) {
		if (chansess->cmd) {
			chansess->original_command = chansess->cmd;
		} else {
			chansess->original_command = m_strdup("");
		}
		chansess->cmd = m_strdup(svr_opts.forced_command);
	} else {
		/* take public key option 'command' into account */
		svr_pubkey_set_forced_command(chansess);
	}


#if LOG_COMMANDS
	if (chansess->cmd) {
		dropbear_log(LOG_INFO, "User %s executing '%s'", 
						ses.authstate.pw_name, chansess->cmd);
	} else {
		dropbear_log(LOG_INFO, "User %s executing login shell", 
						ses.authstate.pw_name);
	}
#endif

#ifdef __ORBIS__
	/* Orbis app sandboxes reject fork() with EPERM. Run remote commands and
	 * the interactive shell in-process while retaining normal channel IO. */
	if (!issubsys) {
		if (iscmd && strncmp(chansess->cmd, "scp", 3) == 0 &&
				(chansess->cmd[3] == '\0' || chansess->cmd[3] == ' ')) {
			ret = ps4_scp_start(channel, chansess);
		} else {
			ret = iscmd ? ps4_builtin_command(channel, chansess)
					: ps4_shell_start(channel, chansess);
		}
		if (ret == DROPBEAR_FAILURE) {
			m_free(chansess->cmd);
			chansess->cmd = NULL;
		}
		return ret;
	}
#endif

	/* uClinux will vfork(), so there'll be a race as 
	connection_string is freed below. */
#if !DROPBEAR_VFORK
	make_connection_string(chansess);
#endif

	if (chansess->term == NULL) {
		/* no pty */
		ret = noptycommand(channel, chansess);
		if (ret == DROPBEAR_SUCCESS) {
			channel->prio = DROPBEAR_PRIO_NORMAL;
			update_channel_prio();
		}
	} else {
		/* want pty */
		ret = ptycommand(channel, chansess);
	}

#if !DROPBEAR_VFORK
	m_free(chansess->connection_string);
	m_free(chansess->client_string);
#endif

	if (ret == DROPBEAR_FAILURE) {
		m_free(chansess->cmd);
		chansess->cmd = NULL;
	}
	TRACE(("leave sessioncommand, ret %d", ret))
	return ret;
}

#ifdef __ORBIS__
static int ps4_resolve_path(const char *cwd, const char *path,
		char *resolved, size_t resolved_size) {
	char combined[PS4_SHELL_PATH_MAX * 2];
	size_t combined_len;
	size_t input_pos = 0;
	size_t output_len = 1;

	if (path == NULL || *path == '\0') {
		path = cwd;
	}
	if (*path == '/') {
		combined_len = (size_t)snprintf(combined, sizeof(combined), "%s", path);
	} else {
		combined_len = (size_t)snprintf(combined, sizeof(combined), "%s/%s", cwd, path);
	}
	if (combined_len >= sizeof(combined) || resolved_size < 2) {
		errno = ENAMETOOLONG;
		return -1;
	}

	resolved[0] = '/';
	resolved[1] = '\0';
	while (combined[input_pos] != '\0') {
		size_t start;
		size_t length;

		while (combined[input_pos] == '/') {
			input_pos++;
		}
		if (combined[input_pos] == '\0') {
			break;
		}
		start = input_pos;
		while (combined[input_pos] != '\0' && combined[input_pos] != '/') {
			input_pos++;
		}
		length = input_pos - start;
		if (length == 1 && combined[start] == '.') {
			continue;
		}
		if (length == 2 && combined[start] == '.' && combined[start + 1] == '.') {
			while (output_len > 1 && resolved[output_len - 1] != '/') {
				output_len--;
			}
			if (output_len > 1) {
				output_len--;
			}
			resolved[output_len] = '\0';
			continue;
		}
		if (output_len > 1) {
			if (output_len + 1 >= resolved_size) {
				errno = ENAMETOOLONG;
				return -1;
			}
			resolved[output_len++] = '/';
		}
		if (output_len + length >= resolved_size) {
			errno = ENAMETOOLONG;
			return -1;
		}
		memcpy(resolved + output_len, combined + start, length);
		output_len += length;
		resolved[output_len] = '\0';
	}
	return 0;
}

static int ps4_list_directory(const char *path, int show_hidden,
		char *output, size_t output_size, int *output_len) {
	struct dirent *entry;
	DIR *directory;
	size_t used = 0;

	directory = opendir(path);
	if (directory == NULL) {
		int len = snprintf(output, output_size, "ls: %s: %s\n", path, strerror(errno));
		if (len < 0) {
			return -1;
		}
		*output_len = MIN(len, (int)output_size - 1);
		return 1;
	}
	while ((entry = readdir(directory)) != NULL) {
		size_t name_len = strlen(entry->d_name);
		int is_directory = entry->d_type == DT_DIR;
		if (!show_hidden && entry->d_name[0] == '.') {
			continue;
		}
		if (used + name_len + (is_directory ? 2 : 1) >= output_size) {
			if (used + 4 < output_size) {
				memcpy(output + used, "...\n", 4);
				used += 4;
			}
			break;
		}
		memcpy(output + used, entry->d_name, name_len);
		used += name_len;
		if (is_directory) {
			output[used++] = '/';
		}
		output[used++] = '\n';
	}
	closedir(directory);
	output[used] = '\0';
	*output_len = (int)used;
	return 0;
}

static int ps4_capture_shell_output(void *ctx, const void *data, size_t len) {
	struct ps4_capture_context *capture = ctx;
	size_t available = capture->size > capture->used ? capture->size - capture->used : 0;

	if (len >= available) {
		if (available > 1) {
			memcpy(capture->output + capture->used, data, available - 1);
			capture->used += available - 1;
		}
		if (capture->size > 0) {
			capture->output[capture->used] = '\0';
		}
		errno = ENOSPC;
		return -1;
	}
	memcpy(capture->output + capture->used, data, len);
	capture->used += len;
	capture->output[capture->used] = '\0';
	return 0;
}

static int ps4_format_command(struct ChanSess *chansess, const char *cmd,
		char *output, size_t output_size, int *output_len) {
	struct ps4_system_version version;
	struct ps4_capture_context capture;
	const char *echo_text;
	int status = 0;
	int len;

	while (*cmd == ' ' || *cmd == '	') {
		cmd++;
	}
	if (strcmp(cmd, "id") == 0) {
		len = snprintf(output, output_size, "uid=0(root) gid=0(root) groups=0(root)\n");
	} else if (strcmp(cmd, "whoami") == 0) {
		len = snprintf(output, output_size, "root\n");
	} else if (strcmp(cmd, "uname") == 0 || strcmp(cmd, "uname -a") == 0) {
		memset(&version, 0, sizeof(version));
		sceKernelGetSystemSwVersion(&version);
		if (strcmp(cmd, "uname -a") == 0) {
			len = snprintf(output, output_size, "OrbisOS %s x86_64\n", version.version);
		} else {
			len = snprintf(output, output_size, "OrbisOS\n");
		}
	} else if (strncmp(cmd, "echo", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
		echo_text = cmd + 4;
		while (*echo_text == ' ') {
			echo_text++;
		}
		len = snprintf(output, output_size, "%s\n", echo_text);
	} else if (strcmp(cmd, "true") == 0) {
		len = 0;
	} else if (strcmp(cmd, "false") == 0) {
		len = 0;
		status = 1;
	} else if (strcmp(cmd, "help") == 0) {
		len = snprintf(output, output_size,
				"Built-ins: pwd cd ls [-la] cat head tail stat hexdump grep find touch "
				"mkdir rmdir rm cp mv chmod id whoami uname echo true false help exit\n"
				"run <elf> [args...] - start an ELF binary as a child process\n");
	} else if (*cmd == '\0') {
		len = 0;
	} else {
		capture.output = output;
		capture.size = output_size;
		capture.used = 0;
		status = ps4_shell_execute(&chansess->ps4_shell, cmd,
				ps4_capture_shell_output, &capture);
		*output_len = (int)capture.used;
		return status;
	}

formatted:
	if (len < 0) {
		return -1;
	}
	if ((size_t)len >= output_size) {
		len = (int)output_size - 1;
	}
	*output_len = len;
	return status;
}

static int ps4_write_all(int fd, const char *data, size_t len) {
	while (len > 0) {
		ssize_t written;
#ifdef MSG_NOSIGNAL
		written = send(fd, data, len, MSG_NOSIGNAL);
		if (written < 0 && errno == ENOTSOCK) {
			written = write(fd, data, len);
		}
#else
		written = write(fd, data, len);
#endif
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			return -1;
		}
		data += written;
		len -= (size_t)written;
	}
	return 0;
}

static int ps4_write_pipe_all(int fd, const char *data, size_t len) {
	while (len > 0) {
		ssize_t written = write(fd, data, len);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			return -1;
		}
		data += written;
		len -= (size_t)written;
	}
	return 0;
}

static int ps4_write_terminal_output(int fd, const char *data, size_t len) {
	size_t start = 0;
	size_t index;

	for (index = 0; index < len; index++) {
		if (data[index] != '\n' || (index > 0 && data[index - 1] == '\r')) {
			continue;
		}
		if (index > start && ps4_write_all(fd, data + start, index - start) != 0) {
			return -1;
		}
		if (ps4_write_all(fd, "\r\n", 2) != 0) {
			return -1;
		}
		start = index + 1;
	}
	if (start < len && ps4_write_all(fd, data + start, len - start) != 0) {
		return -1;
	}
	return 0;
}

static int ps4_terminal_shell_output(void *ctx, const void *data, size_t len) {
	int fd = *(int *)ctx;
	return ps4_write_terminal_output(fd, data, len);
}

static void ps4_write_prompt(int fd, const struct ChanSess *chansess) {
	char prompt[PS4_SHELL_PATH_MAX + 8];
	int len = snprintf(prompt, sizeof(prompt), "ps4:%s# ", chansess->ps4_shell.cwd);
	if (len > 0) {
		(void)ps4_write_all(fd, prompt, MIN((size_t)len, sizeof(prompt) - 1));
	}
}

static int ps4_builtin_command(struct Channel *channel, struct ChanSess *chansess) {
	char output[4096];
	int outfds[2];
	int status;
	int len;

	status = ps4_format_command(chansess, chansess->cmd, output, sizeof(output), &len);
	if (status < 0 || pipe(outfds) != 0) {
		return DROPBEAR_FAILURE;
	}
	if (len > 0 && ps4_write_pipe_all(outfds[1], output, (size_t)len) != 0) {
		m_close(outfds[0]);
		m_close(outfds[1]);
		return DROPBEAR_FAILURE;
	}
	m_close(outfds[1]);

	channel->writefd = open("/dev/null", O_WRONLY);
	if (channel->writefd < 0) {
		m_close(outfds[0]);
		return DROPBEAR_FAILURE;
	}
	channel->readfd = outfds[0];
	channel->errfd = -1;
	channel->bidir_fd = 0;
	channel->prio = DROPBEAR_PRIO_NORMAL;
	ses.maxfd = MAX(ses.maxfd, channel->writefd);
	ses.maxfd = MAX(ses.maxfd, channel->readfd);

	chansess->exit.exitpid = 0;
	chansess->exit.exitstatus = status;
	chansess->exit.exitsignal = -1;
	chansess->exit.exitcore = 0;
	__atomic_store_n(&chansess->ps4_backend_state, PS4_BACKEND_EXEC_READY,
			__ATOMIC_RELEASE);
	return DROPBEAR_SUCCESS;
}

static void *ps4_shell_thread(void *arg) {
	struct ps4_shell_context *context = arg;
	struct ChanSess *chansess = context->chansess;
	char command[512];
	char output[4096];
	char byte;
	size_t command_len = 0;
	int status = 0;
	int output_len;
	int last_was_cr = 0;
	int fd = context->fd;

	free(context);
	(void)ps4_write_all(fd, "Dropbear SSH for PS4 by RuxaXa\r\nType 'help' for commands.\r\n",
			sizeof("Dropbear SSH for PS4 by RuxaXa\r\nType 'help' for commands.\r\n") - 1);
	ps4_write_prompt(fd, chansess);
	while (read(fd, &byte, 1) == 1) {
		if (byte == '\r' || byte == '\n') {
			if (byte == '\n' && last_was_cr) {
				last_was_cr = 0;
				continue;
			}
			last_was_cr = (byte == '\r');
			(void)ps4_write_all(fd, "\r\n", 2);
			command[command_len] = '\0';
			if (strcmp(command, "exit") == 0 || strcmp(command, "logout") == 0) {
				(void)ps4_write_all(fd, "logout\r\n", 8);
				break;
			}
			if (ps4_shell_handles(command)) {
				status = ps4_shell_execute(&chansess->ps4_shell, command,
						ps4_terminal_shell_output, &fd);
				output_len = 0;
			} else {
				status = ps4_format_command(chansess, command, output, sizeof(output), &output_len);
			}
			if (status < 0) {
				status = 1;
				output_len = 0;
			}
			if (output_len > 0) {
				(void)ps4_write_terminal_output(fd, output, (size_t)output_len);
			}
			command_len = 0;
			ps4_write_prompt(fd, chansess);
		} else if (byte == 0x7f || byte == '\b') {
			last_was_cr = 0;
			if (command_len > 0) {
				command_len--;
				(void)ps4_write_all(fd, "\b \b", 3);
			}
		} else if (byte == 0x04 && command_len == 0) {
			(void)ps4_write_all(fd, "logout\r\n", 8);
			break;
		} else if (byte == 0x03) {
			last_was_cr = 0;
			command_len = 0;
			status = 130;
			(void)ps4_write_all(fd, "^C\r\n", sizeof("^C\r\n") - 1);
			ps4_write_prompt(fd, chansess);
		} else if ((unsigned char)byte >= 0x20 && command_len < sizeof(command) - 1) {
			last_was_cr = 0;
			command[command_len++] = byte;
			(void)ps4_write_all(fd, &byte, 1);
		}
	}

	chansess->exit.exitpid = 0;
	chansess->exit.exitstatus = status;
	chansess->exit.exitsignal = -1;
	chansess->exit.exitcore = 0;
	__atomic_store_n(&chansess->ps4_backend_state, PS4_BACKEND_DONE,
			__ATOMIC_RELEASE);
	if (__atomic_exchange_n(&chansess->ps4_backend_fd, -1,
			__ATOMIC_ACQ_REL) >= 0) {
		close(fd);
	}
	return NULL;
}

static int ps4_shell_start(struct Channel *channel, struct ChanSess *chansess) {
	struct ps4_shell_context *context;
	int sockets[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
		return DROPBEAR_FAILURE;
	}
	context = malloc(sizeof(*context));
	if (context == NULL) {
		m_close(sockets[0]);
		m_close(sockets[1]);
		return DROPBEAR_FAILURE;
	}
	context->fd = sockets[1];
	context->chansess = chansess;
	__atomic_store_n(&chansess->ps4_backend_fd, sockets[1], __ATOMIC_RELEASE);
	__atomic_store_n(&chansess->ps4_backend_state, PS4_BACKEND_RUNNING,
			__ATOMIC_RELEASE);
	chansess->ps4_thread_started = 1;
	if (pthread_create(&chansess->ps4_thread, NULL, ps4_shell_thread, context) != 0) {
		chansess->ps4_thread_started = 0;
		__atomic_store_n(&chansess->ps4_backend_state, PS4_BACKEND_IDLE,
				__ATOMIC_RELEASE);
		__atomic_store_n(&chansess->ps4_backend_fd, -1, __ATOMIC_RELEASE);
		free(context);
		m_close(sockets[0]);
		m_close(sockets[1]);
		return DROPBEAR_FAILURE;
	}
	channel->writefd = sockets[0];
	channel->readfd = sockets[0];
	channel->errfd = -1;
	channel->bidir_fd = 1;
	channel->prio = DROPBEAR_PRIO_LOWDELAY;
	ses.maxfd = MAX(ses.maxfd, sockets[0]);
	return DROPBEAR_SUCCESS;
}

static void *ps4_scp_thread(void *arg) {
	struct ps4_scp_context *context = arg;
	struct ChanSess *chansess = context->chansess;
	struct ps4_scp_request request = context->request;
	int fd = context->fd;
	int status;

	free(context);
	status = ps4_scp_serve(fd, &request);
	ps4_runtime_errno_marker(status == 0 ? "scp-worker-done" : "scp-worker-failed", errno);
	chansess->exit.exitpid = 0;
	chansess->exit.exitstatus = status == 0 ? 0 : 1;
	chansess->exit.exitsignal = -1;
	chansess->exit.exitcore = 0;
	__atomic_store_n(&chansess->ps4_backend_state, PS4_BACKEND_DONE,
			__ATOMIC_RELEASE);
	if (__atomic_exchange_n(&chansess->ps4_backend_fd, -1,
			__ATOMIC_ACQ_REL) >= 0) {
		close(fd);
	}
	return NULL;
}

static int ps4_scp_start(struct Channel *channel, struct ChanSess *chansess) {
	struct ps4_scp_context *context;
	struct ps4_scp_request request;
	int sockets[2];

	if (ps4_scp_parse_command(chansess->cmd, chansess->ps4_shell.cwd, &request) != 0) {
		return DROPBEAR_FAILURE;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
		return DROPBEAR_FAILURE;
	}
	context = malloc(sizeof(*context));
	if (context == NULL) {
		m_close(sockets[0]);
		m_close(sockets[1]);
		return DROPBEAR_FAILURE;
	}
	context->fd = sockets[1];
	context->chansess = chansess;
	context->request = request;
	__atomic_store_n(&chansess->ps4_backend_fd, sockets[1], __ATOMIC_RELEASE);
	__atomic_store_n(&chansess->ps4_backend_state, PS4_BACKEND_RUNNING,
			__ATOMIC_RELEASE);
	chansess->ps4_thread_started = 1;
	if (pthread_create(&chansess->ps4_thread, NULL, ps4_scp_thread, context) != 0) {
		chansess->ps4_thread_started = 0;
		__atomic_store_n(&chansess->ps4_backend_state, PS4_BACKEND_IDLE,
				__ATOMIC_RELEASE);
		__atomic_store_n(&chansess->ps4_backend_fd, -1, __ATOMIC_RELEASE);
		free(context);
		m_close(sockets[0]);
		m_close(sockets[1]);
		return DROPBEAR_FAILURE;
	}
	channel->writefd = sockets[0];
	channel->readfd = sockets[0];
	channel->errfd = -1;
	channel->bidir_fd = 1;
	channel->prio = DROPBEAR_PRIO_NORMAL;
	ses.maxfd = MAX(ses.maxfd, sockets[0]);
	return DROPBEAR_SUCCESS;
}

#endif

/* Execute a command and set up redirection of stdin/stdout/stderr without a
 * pty.
 * Returns DROPBEAR_SUCCESS or DROPBEAR_FAILURE */
static int noptycommand(struct Channel *channel, struct ChanSess *chansess) {
	int ret;

	TRACE(("enter noptycommand"))
	ret = spawn_command(execchild, chansess, 
			&channel->writefd, &channel->readfd, &channel->errfd,
			&chansess->pid);

	if (ret == DROPBEAR_FAILURE) {
		return ret;
	}

	ses.maxfd = MAX(ses.maxfd, channel->writefd);
	ses.maxfd = MAX(ses.maxfd, channel->readfd);
	ses.maxfd = MAX(ses.maxfd, channel->errfd);
	channel->bidir_fd = 0;

	addchildpid(chansess, chansess->pid);

	if (svr_ses.lastexit.exitpid != -1) {
		unsigned int i;
		TRACE(("parent side: lastexitpid is %d", svr_ses.lastexit.exitpid))
		/* The child probably exited and the signal handler triggered
		 * possibly before we got around to adding the childpid. So we fill
		 * out its data manually */
		for (i = 0; i < svr_ses.childpidsize; i++) {
			if (svr_ses.childpids[i].pid == svr_ses.lastexit.exitpid) {
				TRACE(("found match for lastexitpid"))
				svr_ses.childpids[i].chansess->exit = svr_ses.lastexit;
				svr_ses.lastexit.exitpid = -1;
				break;
			}
		}
	}

	TRACE(("leave noptycommand"))
	return DROPBEAR_SUCCESS;
}

/* Execute a command or shell within a pty environment, and set up
 * redirection as appropriate.
 * Returns DROPBEAR_SUCCESS or DROPBEAR_FAILURE */
static int ptycommand(struct Channel *channel, struct ChanSess *chansess) {

	pid_t pid;
	struct logininfo *li = NULL;
#if DO_MOTD
	buffer * motdbuf = NULL;
	int len;
	struct stat sb;
	char *hushpath = NULL;
#endif

	TRACE(("enter ptycommand"))

	/* we need to have a pty allocated */
	if (chansess->master == -1 || chansess->tty == NULL) {
		dropbear_log(LOG_WARNING, "No pty was allocated, couldn't execute");
		return DROPBEAR_FAILURE;
	}
	
#ifdef __ORBIS__
	pid = ps4_fork();
#elif DROPBEAR_VFORK
	pid = vfork();
#else
	pid = fork();
#endif
	if (pid < 0) {
#ifdef __ORBIS__
		ps4_runtime_errno_marker("pty-command-fork-failed", errno);
#endif
		return DROPBEAR_FAILURE;
	}

	if (pid == 0) {
		/* child */
		
		TRACE(("back to normal sigchld"))
		/* Revert to normal sigchld handling */
#ifndef __ORBIS__
		if (signal(SIGCHLD, SIG_DFL) == SIG_ERR) {
			dropbear_exit("signal() error");
		}
#endif
		
		/* redirect stdin/stdout/stderr */
		close(chansess->master);

		pty_make_controlling_tty(&chansess->slave, chansess->tty);
		
		if ((dup2(chansess->slave, STDIN_FILENO) < 0) ||
			(dup2(chansess->slave, STDOUT_FILENO) < 0)) {
			TRACE(("leave ptycommand: error redirecting filedesc"))
			return DROPBEAR_FAILURE;
			}

		/* write the utmp/wtmp login record - must be after changing the
		 * terminal used for stdout with the dup2 above, otherwise
		 * the wtmp login will not be recorded */
		li = chansess_login_alloc(chansess);

		svr_raise_gid_utmp();
		login_login(li);
		svr_restore_gid();

		login_free_entry(li);

		/* Can now dup2 stderr. Messages from login_login() have gone
		to the parent stderr */
		if (dup2(chansess->slave, STDERR_FILENO) < 0) {
			TRACE(("leave ptycommand: error redirecting filedesc"))
			return DROPBEAR_FAILURE;
		}

		close(chansess->slave);

#if DO_MOTD
		if (svr_opts.domotd && !chansess->cmd) {
			/* don't show the motd if ~/.hushlogin exists */

			/* 12 == strlen("/.hushlogin\0") */
			len = strlen(ses.authstate.pw_dir) + 12; 

			hushpath = m_malloc(len);
			snprintf(hushpath, len, "%s/.hushlogin", ses.authstate.pw_dir);

			if (stat(hushpath, &sb) < 0) {
				char *expand_path = NULL;
				/* more than a screenful is stupid IMHO */
				motdbuf = buf_new(MOTD_MAXSIZE);
				expand_path = expand_homedir_path(MOTD_FILENAME);
				if (buf_readfile(motdbuf, expand_path) == DROPBEAR_SUCCESS) {
					/* incase it is full size, add LF at last position */
					if (motdbuf->len == motdbuf->size) motdbuf->data[motdbuf->len - 1]=10;
					buf_setpos(motdbuf, 0);
					while (motdbuf->pos != motdbuf->len) {
						len = motdbuf->len - motdbuf->pos;
						len = write(STDOUT_FILENO, 
								buf_getptr(motdbuf, len), len);
						buf_incrpos(motdbuf, len);
					}
				}
				m_free(expand_path);
				buf_free(motdbuf);

			}
			m_free(hushpath);
		}
#endif /* DO_MOTD */

		execchild(chansess);
		/* not reached */

	} else {
		/* parent */
		TRACE(("continue ptycommand: parent"))
		chansess->pid = pid;

		/* add a child pid */
		addchildpid(chansess, pid);

		close(chansess->slave);
		channel->writefd = chansess->master;
		channel->readfd = chansess->master;
		/* don't need to set stderr here */
		ses.maxfd = MAX(ses.maxfd, chansess->master);
		channel->bidir_fd = 0;

		setnonblocking(chansess->master);

	}

	TRACE(("leave ptycommand"))
	return DROPBEAR_SUCCESS;
}

/* Add the pid of a child to the list for exit-handling */
static void addchildpid(struct ChanSess *chansess, pid_t pid) {

	unsigned int i;
	for (i = 0; i < svr_ses.childpidsize; i++) {
		if (svr_ses.childpids[i].pid == -1) {
			break;
		}
	}

	/* need to increase size */
	if (i == svr_ses.childpidsize) {
		svr_ses.childpids = (struct ChildPid*)m_realloc(svr_ses.childpids,
				sizeof(struct ChildPid) * (svr_ses.childpidsize+1));
		svr_ses.childpidsize++;
	}
	
	TRACE(("addchildpid %d pid %d for chansess %p", i, pid, chansess))
	svr_ses.childpids[i].pid = pid;
	svr_ses.childpids[i].chansess = chansess;

}

/* Clean up, drop to user privileges, set up the environment and execute
 * the command/shell. This function does not return. */
static void execchild(const void *user_data) {
	const struct ChanSess *chansess = user_data;
	char *usershell = NULL;
	char *cp = NULL;
	char *envcp = getenv("LANG");
	if (envcp != NULL) {
		cp = m_strdup(envcp);
	}

	/* with uClinux we'll have vfork()ed, so don't want to overwrite the
	 * hostkey. can't think of a workaround to clear it */
#if !DROPBEAR_VFORK
	/* wipe the hostkey */
	sign_key_free(svr_opts.hostkey);
	svr_opts.hostkey = NULL;

	/* overwrite the prng state */
	seedrandom();
#endif

	/* clear environment if -e was not set */
	/* if we're debugging using valgrind etc, we need to keep the LD_PRELOAD
	 * etc. This is hazardous, so should only be used for debugging. */
	if ( !svr_opts.pass_on_env) {
#ifndef DEBUG_VALGRIND
#ifdef HAVE_CLEARENV
		clearenv();
#else /* don't HAVE_CLEARENV */
		/* Yay for posix. */
		if (environ) {
			environ[0] = NULL;
		}
#endif /* HAVE_CLEARENV */
#endif /* DEBUG_VALGRIND */
	}

#if !DROPBEAR_SVR_DROP_PRIVS
	svr_switch_user();
#endif

	/* set env vars */
	addnewvar("USER", ses.authstate.pw_name);
	addnewvar("LOGNAME", ses.authstate.pw_name);
	addnewvar("HOME", ses.authstate.pw_dir);
	addnewvar("SHELL", get_user_shell());
	if (getuid() == 0) {
		addnewvar("PATH", DEFAULT_ROOT_PATH);
	} else {
		addnewvar("PATH", DEFAULT_PATH);
	}
	if (cp != NULL) {
		addnewvar("LANG", cp);
		m_free(cp);
	}	
	if (chansess->term != NULL) {
		addnewvar("TERM", chansess->term);
	}

	if (chansess->tty) {
		addnewvar("SSH_TTY", chansess->tty);
	}
	
	if (chansess->connection_string) {
		addnewvar("SSH_CONNECTION", chansess->connection_string);
	}

	if (chansess->client_string) {
		addnewvar("SSH_CLIENT", chansess->client_string);
	}
	
	if (chansess->original_command) {
		addnewvar("SSH_ORIGINAL_COMMAND", chansess->original_command);
	}
#if DROPBEAR_SVR_PUBKEY_OPTIONS_BUILT
	if (ses.authstate.pubkey_options
		&& ses.authstate.pubkey_options->info_env) {
		addnewvar("SSH_PUBKEYINFO", ses.authstate.pubkey_options->info_env);
	}
#endif

	/* change directory */
	if (chdir(ses.authstate.pw_dir) < 0) {
		int e = errno;
		if (chdir("/") < 0) {
			dropbear_exit("chdir(\"/\") failed");
		}
		fprintf(stderr, "Failed chdir '%s': %s\n", ses.authstate.pw_dir, strerror(e));
	}


#if DROPBEAR_X11FWD
	/* set up X11 forwarding if enabled */
	x11setauth(chansess);
#endif
#if DROPBEAR_SVR_AGENTFWD
	/* set up agent env variable */
	svr_agentset(chansess);
#endif

	usershell = m_strdup(get_user_shell());
	run_shell_command(chansess->cmd, ses.maxfd, usershell);

	/* only reached on error */
	dropbear_exit("Child failed");
}

/* Set up the general chansession environment, in particular child-exit
 * handling */
void svr_chansessinitialise() {

	struct sigaction sa_chld;

	/* single child process intially */
	svr_ses.childpids = (struct ChildPid*)m_malloc(sizeof(struct ChildPid));
	svr_ses.childpids[0].pid = -1; /* unused */
	svr_ses.childpids[0].chansess = NULL;
	svr_ses.childpidsize = 1;
	svr_ses.lastexit.exitpid = -1; /* Nothing has exited yet */
#ifndef __ORBIS__
	sa_chld.sa_handler = sesssigchild_handler;
	sa_chld.sa_flags = SA_NOCLDSTOP;
	sigemptyset(&sa_chld.sa_mask);
	if (sigaction(SIGCHLD, &sa_chld, NULL) < 0) {
		dropbear_exit("signal() error");
	}
#else
	/* PS4 app sandboxes deny SIGCHLD registration and cannot fork session
	 * children, so no child-exit handler is required in serial mode. */
	(void)sa_chld;
#endif
	
}

/* add a new environment variable, allocating space for the entry */
void addnewvar(const char* param, const char* var) {

	char* newvar = NULL;
	int plen, vlen;

	plen = strlen(param);
	vlen = strlen(var);

	newvar = m_malloc(plen + vlen + 2); /* 2 is for '=' and '\0' */
	memcpy(newvar, param, plen);
	newvar[plen] = '=';
	memcpy(&newvar[plen+1], var, vlen);
	newvar[plen+vlen+1] = '\0';
	/* newvar is leaked here, but that's part of putenv()'s semantics */
	if (putenv(newvar) < 0) {
		dropbear_exit("environ error");
	}
}
