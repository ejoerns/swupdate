/*
 * (C) Copyright 2012-2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>

#include "bsdqueue.h"
#include "util.h"
#include "network_ipc.h"
#include "network_interface.h"
#include "installer.h"
#include "swupdate.h"

#define LISTENQ	1024

#define NUM_CACHED_MESSAGES 100

struct msg_elem {
	RECOVERY_STATUS status;
	int error;
	char *msg;
	SIMPLEQ_ENTRY(msg_elem) next;
};

SIMPLEQ_HEAD(msglist, msg_elem);
static struct msglist notifymsgs;
static unsigned long nrmsgs = 0;

static pthread_mutex_t msglock = PTHREAD_MUTEX_INITIALIZER;

static void clean_msg(char *msg, char drop)
{
	char *lfpos;
	lfpos = strchr(msg, drop);
	while (lfpos) {
		*lfpos = ' ';
		lfpos = strchr(msg, drop);
	}
}

static void network_notifier(RECOVERY_STATUS status, int error, const char *msg)
{
	int len = msg ? strlen(msg) : 0;
	struct msg_elem *newmsg = (struct msg_elem *)calloc(1, sizeof(*newmsg) + len + 1);
	struct msg_elem *oldmsg;

	if (!newmsg)
		return;

	pthread_mutex_lock(&msglock);
	nrmsgs++;
	if (nrmsgs > NUM_CACHED_MESSAGES) {
		oldmsg = SIMPLEQ_FIRST(&notifymsgs);
		SIMPLEQ_REMOVE_HEAD(&notifymsgs, next);
		free(oldmsg);
		nrmsgs--;
	}
	newmsg->msg = (char *)newmsg + sizeof(struct msg_elem);

	newmsg->status = status;
	newmsg->error = error;

	if (msg) {
		strncpy(newmsg->msg, msg, len);
		clean_msg(newmsg->msg, '\t');
		clean_msg(newmsg->msg, '\n');
		clean_msg(newmsg->msg, '\r');
	}


	SIMPLEQ_INSERT_TAIL(&notifymsgs, newmsg, next);
	pthread_mutex_unlock(&msglock);
}

int listener_create(const char *path, int type)
{
	struct sockaddr_un servaddr;
	int listenfd;

	listenfd = socket(AF_LOCAL, type, 0);
	unlink(path);
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sun_family = AF_LOCAL;
	strcpy(servaddr.sun_path, path);

	if (bind(listenfd,  (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0) {
		close(listenfd);
		return -1;
	}

	chmod(path,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

	if (type == SOCK_STREAM)
		if (listen(listenfd, LISTENQ) < 0) {
			close(listenfd);
			return -1;
		}
	return listenfd;

}

static void cleanum_msg_list(void)
{
	struct msg_elem *notification;

	pthread_mutex_lock(&msglock);

	while (!SIMPLEQ_EMPTY(&notifymsgs)) {
		notification = SIMPLEQ_FIRST(&notifymsgs);
		SIMPLEQ_REMOVE_HEAD(&notifymsgs, next);
		free(notification);
	}
	nrmsgs = 0;
	pthread_mutex_unlock(&msglock);
}

void *network_thread (void *data)
{
	struct installer *instp = (struct installer *)data;
	int ctrllisten, ctrlconnfd;
	socklen_t clilen;
	struct sockaddr_un cliaddr;
	ipc_message msg;
	int nread;
	struct msg_elem *notification;
	int ret;

	if (!instp) {
		TRACE("Fatal error: Network thread aborting...");
		return (void *)0;
	}

	SIMPLEQ_INIT(&notifymsgs);
	register_notifier(network_notifier);

	/* Initialize and bind to UDS */
	ctrllisten = listener_create(SOCKET_CTRL_PATH, SOCK_STREAM);
	if (ctrllisten < 0 ) {
		TRACE("Error creating IPC sockets");
		exit(2);
	}

	do {
		clilen = sizeof(cliaddr);
		if ( (ctrlconnfd = accept(ctrllisten, (struct sockaddr *) &cliaddr, &clilen)) < 0) {
			if (errno == EINTR)
				continue;
			else {
				TRACE("Accept returns: %s", strerror(errno));
				continue;
			}
		}
		nread = read(ctrlconnfd, (void *)&msg, sizeof(msg));

		if (nread != sizeof(msg)) {
			TRACE("IPC message too short: fragmentation not supported");
			close(ctrlconnfd);
			continue;
		}
#ifdef DEBUG_IPC
		TRACE("request header: magic[0x%08X] type[0x%08X]", msg.magic, msg.type);
#endif

		pthread_mutex_lock(&stream_mutex);
		if (msg.magic == IPC_MAGIC)  {
			switch (msg.type) {
			case POST_UPDATE:
				if (postupdate(get_swupdate_cfg()) == 0) {
					msg.type = ACK;
					sprintf(msg.data.msg, "Post-update actions successfully executed.");
				} else {
					msg.type = NACK;
					sprintf(msg.data.msg, "Post-update actions failed.");
				}
				break;
			case REQ_INSTALL:
				TRACE("Incoming network request: processing...");
				if (instp->status == IDLE) {
					instp->fd = ctrlconnfd;
					instp->source = msg.data.instmsg.source;
					instp->len = min(msg.data.instmsg.len, sizeof(instp->info));
					memcpy(instp->info, msg.data.instmsg.buf,
						instp->len);

					/*
					 * Prepare answer
					 */
					msg.type = ACK;

					/* Drop all old notification from last run */
					cleanum_msg_list();

					/* Wake-up the installer */
					pthread_cond_signal(&stream_wkup);
				} else {
					msg.type = NACK;
					sprintf(msg.data.msg, "Installation in progress");
				}
				break;
			case GET_STATUS:
				msg.type = GET_STATUS;
				memset(msg.data.msg, 0, sizeof(msg.data.msg));
				msg.data.status.current = instp->status;
				msg.data.status.last_result = instp->last_install;
				msg.data.status.error = instp->last_error;

				/* Get first notification from the queue */
				pthread_mutex_lock(&msglock);
				notification = SIMPLEQ_FIRST(&notifymsgs);
				if (notification) {
					SIMPLEQ_REMOVE_HEAD(&notifymsgs, next);
					nrmsgs--;
					strncpy(msg.data.status.desc, notification->msg,
						sizeof(msg.data.status.desc) - 1);
#ifdef DEBUG_IPC
					printf("GET STATUS: %s\n", msg.data.status.desc);
#endif
					msg.data.status.current = notification->status;
					msg.data.status.error = notification->error;
				}
				pthread_mutex_unlock(&msglock);

				break;
			default:
				msg.type = NACK;
			}
		} else {
			/* Wrong request */
			msg.type = NACK;
			sprintf(msg.data.msg, "Wrong request: aborting");
		}
		ret = write(ctrlconnfd, &msg, sizeof(msg));
		if (ret < 0)
			printf("Error write on socket ctrl");

		if (msg.type != ACK)
			close(ctrlconnfd);
		pthread_mutex_unlock(&stream_mutex);
	} while (1);
	return (void *)0; 
}
