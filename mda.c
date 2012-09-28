/*	$OpenBSD: mda.c,v 1.77 2012/09/27 17:47:49 chl Exp $	*/

/*
 * Copyright (c) 2008 Gilles Chehade <gilles@openbsd.org>
 * Copyright (c) 2008 Pierre-Yves Ritschard <pyr@openbsd.org>
 * Copyright (c) 2009 Jacek Masiulaniec <jacekm@dobremiasto.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/param.h>
#include <sys/socket.h>

#include <ctype.h>
#include <err.h>
#include <event.h>
#include <imsg.h>
#include <inttypes.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vis.h>

#include "smtpd.h"
#include "log.h"

struct mda_session {
	struct envelope		 msg;
	struct msgbuf		 w;
	struct event		 ev;
	FILE			*datafp;
};

static void mda_imsg(struct imsgev *, struct imsg *);
static void mda_shutdown(void);
static void mda_sig_handler(int, short, void *);
static void mda_store(struct mda_session *);
static void mda_store_event(int, short, void *);
static int mda_check_loop(FILE *, struct envelope *);

static struct tree	sessions;
static uint32_t		mda_id = 0;

static void
mda_imsg(struct imsgev *iev, struct imsg *imsg)
{
	char			 output[128], *error, *parent_error;
	char			 stat[MAX_LINE_SIZE];
	struct deliver		 deliver;
	struct mda_session	*s;
	struct delivery_mda	*d_mda;
	struct envelope		*ep;
	FILE			*fp;
	uint16_t		 msg;
	uint32_t		 id;

	if (iev->proc == PROC_QUEUE) {
		switch (imsg->hdr.type) {
		case IMSG_MDA_SESS_NEW:
			ep = (struct envelope *)imsg->data;
			fp = fdopen(imsg->fd, "r");
			if (fp == NULL)
				fatalx("mda: fdopen");

			if (mda_check_loop(fp, ep)) {
				log_debug("mda: loop detected");
				envelope_set_errormsg(ep, "646 loop detected");
				imsg_compose_event(env->sc_ievs[PROC_QUEUE],
				    IMSG_QUEUE_DELIVERY_LOOP, 0, 0, -1, ep,
				    sizeof *ep);
				fclose(fp);
				return;
			}

			/* make new session based on provided args */
			s = xcalloc(1, sizeof *s, "mda_imsg");
			msgbuf_init(&s->w);
			s->msg = *ep;
			s->datafp = fp;
			id = mda_id++;
			tree_xset(&sessions, id, s);

			/* request parent to fork a helper process */
			ep    = &s->msg;
			d_mda = &s->msg.agent.mda;
			switch (d_mda->method) {
			case A_MDA:
				deliver.mode = A_MDA;
				strlcpy(deliver.user, d_mda->user,
				    sizeof (deliver.user));
				strlcpy(deliver.to, d_mda->buffer,
				    sizeof deliver.to);
				break;
				
			case A_MBOX:
				deliver.mode = A_MBOX;
				strlcpy(deliver.user, "root",
				    sizeof (deliver.user));
				strlcpy(deliver.to, d_mda->user,
				    sizeof (deliver.to));
				snprintf(deliver.from, sizeof(deliver.from),
				    "%s@%s", ep->sender.user,
				    ep->sender.domain);
				break;

			case A_MAILDIR:
				deliver.mode = A_MAILDIR;
				strlcpy(deliver.user, d_mda->user,
				    sizeof deliver.user);
				strlcpy(deliver.to, d_mda->buffer,
				    sizeof deliver.to);
				break;

			case A_FILENAME:
				deliver.mode = A_FILENAME;
				strlcpy(deliver.user, d_mda->user,
				    sizeof deliver.user);
				strlcpy(deliver.to, d_mda->buffer,
				    sizeof deliver.to);
				break;

			default:
				errx(1, "mda: unknown delivery method: %d",
				    d_mda->method);
			}

			imsg_compose_event(env->sc_ievs[PROC_PARENT],
			    IMSG_PARENT_FORK_MDA, id, 0, -1, &deliver,
			    sizeof deliver);
			return;
		}
	}

	if (iev->proc == PROC_PARENT) {
		switch (imsg->hdr.type) {
		case IMSG_PARENT_FORK_MDA:
			s = tree_xget(&sessions, imsg->hdr.peerid);
			if (imsg->fd < 0)
				fatalx("mda: fd pass fail");
			s->w.fd = imsg->fd;
			mda_store(s);
			return;

		case IMSG_MDA_DONE:
			s = tree_xpop(&sessions, imsg->hdr.peerid);
			/*
			 * Grab last line of mda stdout/stderr if available.
			 */
			output[0] = '\0';
			if (imsg->fd != -1) {
				char *ln, *buf;
				size_t len;

				buf = NULL;
				if (lseek(imsg->fd, 0, SEEK_SET) < 0)
					fatalx("lseek");
				fp = fdopen(imsg->fd, "r");
				if (fp == NULL)
					fatal("mda: fdopen");
				while ((ln = fgetln(fp, &len))) {
					if (ln[len - 1] == '\n')
						ln[len - 1] = '\0';
					else {
						buf = xmalloc(len + 1,
						    "mda_imsg");
						memcpy(buf, ln, len);
						buf[len] = '\0';
						ln = buf;
					}
					strlcpy(output, "\"", sizeof output);
					strnvis(output + 1, ln,
					    sizeof(output) - 2,
					    VIS_SAFE | VIS_CSTYLE);
					strlcat(output, "\"", sizeof output);
					log_debug("mda_out: %s", output);
				}
				free(buf);
				fclose(fp);
			}

			/*
			 * Choose between parent's description of error and
			 * child's output, the latter having preference over
			 * the former.
			 */
			error = NULL;
			parent_error = imsg->data;
			if (strcmp(parent_error, "exited okay") == 0) {
				if (!feof(s->datafp) || s->w.queued)
					error = "mda exited prematurely";
			} else {
				if (output[0])
					error = output;
				else
					error = parent_error;
			}

			/* update queue entry */
			msg = IMSG_QUEUE_DELIVERY_OK;
			if (error) {
				msg = IMSG_QUEUE_DELIVERY_TEMPFAIL;
				envelope_set_errormsg(&s->msg, "%s", error);
				snprintf(stat, sizeof stat, "Error (%s)", error);
			}
			imsg_compose_event(env->sc_ievs[PROC_QUEUE], msg,
			    0, 0, -1, &s->msg, sizeof s->msg);

			log_envelope(&s->msg, NULL, error ? stat : "Delivered");

			/* destroy session */
			if (s->w.fd != -1)
				close(s->w.fd);
			if (s->datafp)
				fclose(s->datafp);
			msgbuf_clear(&s->w);
			event_del(&s->ev);
			free(s);
			return;

		case IMSG_CTL_VERBOSE:
			log_verbose(*(int *)imsg->data);
			return;
		}
	}

	errx(1, "mda_imsg: unexpected %s imsg", imsg_to_str(imsg->hdr.type));
}

static void
mda_sig_handler(int sig, short event, void *p)
{
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		mda_shutdown();
		break;
	default:
		fatalx("mda_sig_handler: unexpected signal");
	}
}

static void
mda_shutdown(void)
{
	log_info("mail delivery agent exiting");
	_exit(0);
}

pid_t
mda(void)
{
	pid_t		 pid;
	struct passwd	*pw;

	struct event	 ev_sigint;
	struct event	 ev_sigterm;

	struct peer peers[] = {
		{ PROC_PARENT,	imsg_dispatch },
		{ PROC_QUEUE,	imsg_dispatch },
		{ PROC_CONTROL,	imsg_dispatch }
	};

	switch (pid = fork()) {
	case -1:
		fatal("mda: cannot fork");
	case 0:
		break;
	default:
		return (pid);
	}

	purge_config(PURGE_EVERYTHING);

	pw = env->sc_pw;

	if (chroot(pw->pw_dir) == -1)
		fatal("mda: chroot");
	if (chdir("/") == -1)
		fatal("mda: chdir(\"/\")");

	smtpd_process = PROC_MDA;
	setproctitle("%s", env->sc_title[smtpd_process]);

	if (setgroups(1, &pw->pw_gid) ||
	    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
	    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
		fatal("mda: cannot drop privileges");

	tree_init(&sessions);

	imsg_callback = mda_imsg;
	event_init();

	signal_set(&ev_sigint, SIGINT, mda_sig_handler, NULL);
	signal_set(&ev_sigterm, SIGTERM, mda_sig_handler, NULL);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	config_pipes(peers, nitems(peers));
	config_peers(peers, nitems(peers));

	if (event_dispatch() < 0)
		fatal("event_dispatch");
	mda_shutdown();

	return (0);
}

static void
mda_store(struct mda_session *s)
{
	char		*p;
	struct ibuf	*buf;
	int		 len;

	if (s->msg.sender.user[0] && s->msg.sender.domain[0])
		/* XXX: remove user provided Return-Path, if any */
		len = asprintf(&p, "Return-Path: %s@%s\nDelivered-To: %s@%s\n",
		    s->msg.sender.user, s->msg.sender.domain,
		    s->msg.rcpt.user,
		    s->msg.rcpt.domain);
	else
		len = asprintf(&p, "Delivered-To: %s@%s\n",
		    s->msg.rcpt.user,
		    s->msg.rcpt.domain);

	if (len == -1)
		fatal("mda_store: asprintf");

	session_socket_blockmode(s->w.fd, BM_NONBLOCK);
	if ((buf = ibuf_open(len)) == NULL)
		fatal(NULL);
	if (ibuf_add(buf, p, len) < 0)
		fatal(NULL);
	ibuf_close(&s->w, buf);
	event_set(&s->ev, s->w.fd, EV_WRITE, mda_store_event, s);
	event_add(&s->ev, NULL);
	free(p);
}

static void
mda_store_event(int fd, short event, void *p)
{
	char			 tmp[16384];
	struct mda_session	*s = p;
	struct ibuf		*buf;
	size_t			 len;

	if (s->w.queued == 0) {
		if ((buf = ibuf_dynamic(0, sizeof tmp)) == NULL)
			fatal(NULL);
		len = fread(tmp, 1, sizeof tmp, s->datafp);
		if (ferror(s->datafp))
			fatal("mda_store_event: fread failed");
		if (feof(s->datafp) && len == 0) {
			close(s->w.fd);
			s->w.fd = -1;
			return;
		}
		if (ibuf_add(buf, tmp, len) < 0)
			fatal(NULL);
		ibuf_close(&s->w, buf);
	}

	if (ibuf_write(&s->w) < 0) {
		close(s->w.fd);
		s->w.fd = -1;
		return;
	}

	event_set(&s->ev, fd, EV_WRITE, mda_store_event, s);
	event_add(&s->ev, NULL);
}

static int
mda_check_loop(FILE *fp, struct envelope *ep)
{
	char		*buf, *lbuf;
	size_t		 len;
	struct mailaddr	 maddr;
	int		 ret = 0;

	lbuf = NULL;
	while ((buf = fgetln(fp, &len))) {
		if (buf[len - 1] == '\n')
			buf[len - 1] = '\0';
		else {
			/* EOF without EOL, copy and add the NUL */
			lbuf = xmalloc(len + 1, "mda_check_loop");
			memcpy(lbuf, buf, len);
			lbuf[len] = '\0';
			buf = lbuf;
		}

		if (strchr(buf, ':') == NULL && !isspace((int)*buf))
			break;

		if (strncasecmp("Delivered-To: ", buf, 14) == 0) {

			bzero(&maddr, sizeof maddr);
			if (! email_to_mailaddr(&maddr, buf + 14))
				continue;
			if (strcasecmp(maddr.user, ep->dest.user) == 0 &&
			    strcasecmp(maddr.domain, ep->dest.domain) == 0) {
				ret = 1;
				break;
			}
		}
		if (lbuf) {
			free(lbuf);
			lbuf = NULL;
		}
	}
	if (lbuf)
		free(lbuf);

	fseek(fp, SEEK_SET, 0);

	return (ret);
}
