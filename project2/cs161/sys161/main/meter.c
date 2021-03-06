#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "config.h"

#include "speed.h"
#include "clock.h"
#include "console.h"
#include "onsel.h"
#include "main.h" /* for g_stats */
#include "meter.h"

#define PROTOCOL_VERSION  1

////////////////////////////////////////////////////////////

struct meter {
	int fd;
};

static int meter_socket = -1;

static
void
meter_hello(struct meter *m)
{
	char buf[4096];
	snprintf(buf, sizeof(buf), 
		 "HELLO %d\r\n", PROTOCOL_VERSION);
	write(m->fd, buf, strlen(buf));
}

static
void
meter_header(struct meter *m)
{
	char buf[4096];
	snprintf(buf, sizeof(buf), 
		 "HEAD kern user idle irqs exns disk con emu net\r\n");
	write(m->fd, buf, strlen(buf));
	snprintf(buf, sizeof(buf),
		 "WIDTH 10 10 10 4 4 4 5 4 4\r\n");
	write(m->fd, buf, strlen(buf));
}

static
void
meter_report(struct meter *m)
{
	char buf[4096];
	char buf2[512];

	if (sizeof(u_int64_t)==sizeof(unsigned long)) {
		snprintf(buf2, sizeof(buf2), "%lu %lu %lu",
			 (unsigned long) g_stats.s_kcycles,
			 (unsigned long) g_stats.s_ucycles,
			 (unsigned long) g_stats.s_icycles);
	}
	else {
		snprintf(buf2, sizeof(buf2), "%llu %llu %llu",
			 (unsigned long long) g_stats.s_kcycles,
			 (unsigned long long) g_stats.s_ucycles,
			 (unsigned long long) g_stats.s_icycles);
	}

	snprintf(buf, sizeof(buf), "DATA %s %lu %lu %lu %lu %lu %lu\r\n", buf2,
		 (unsigned long) g_stats.s_irqs,
		 (unsigned long) g_stats.s_exns,
		 (unsigned long) (g_stats.s_rsects + g_stats.s_wsects),
		 (unsigned long) (g_stats.s_rchars + g_stats.s_wchars),
		 (unsigned long) (g_stats.s_remu + g_stats.s_wemu + 
				  g_stats.s_memu),
		 (unsigned long) (g_stats.s_rpkts + g_stats.s_wpkts));

	write(m->fd, buf, strlen(buf));
}

static
void
meter_update(void *x, u_int32_t junk)
{
	struct meter *m = x;

	(void)junk;

	if (m->fd < 0) {
		free(m);
		return;
	}

	meter_report(m);
	schedule_event(METER_NSECS, m, 0, meter_update, "perfmeter");
}

static
int
meter_receive(void *x)
{
	struct meter *m = x;
	char buf[128];
	int r;

	r = read(m->fd, buf, sizeof(buf));
	if (r<=0) {
		/* error/EOF? close connection; m will be freed next update */
		close(m->fd);
		m->fd = -1;
		return -1;
	}

	/* otherwise, ignore anything sent to us */
	return 0;
}

////////////////////////////////////////////////////////////
//
// Setup code

static
int
meter_accept(void *x)
{
	struct meter *m;
	struct sockaddr_storage sa;
	socklen_t salen;
	int remotefd;

	(void)x; /* not used */

	salen = sizeof(sa);
	remotefd = accept(meter_socket, (struct sockaddr *)&sa, &salen);
	if (remotefd < 0) {
		/* ...? */
		return 0;
	}

	m = malloc(sizeof(struct meter));
	if (m==NULL) {
		msg("malloc failed allocating meter data for new connection");
		write(remotefd, "ERROR\r\n", 7);
		close(remotefd);
		return 0;
	}

	m->fd = remotefd;
	onselect(remotefd, m, meter_receive, NULL);

	meter_hello(m);
	meter_header(m);
	meter_update(m, 0);

	return 0;
}

static
int
meter_listen(const char *name)
{
	struct sockaddr_un su;
	socklen_t len;
	int sfd;

	sfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sfd < 0) {
		msg("socket: %s", strerror(errno));
		return -1;
	}

	memset(&su, 0, sizeof(su));
	su.sun_family = AF_UNIX;
	snprintf(su.sun_path, sizeof(su.sun_path), "%s", name);
	len = SUN_LEN(&su);
#ifdef HAS_SUN_LEN
	su.sun_len = len;
#endif

	if (bind(sfd, (struct sockaddr *) &su, len) < 0) {
		msg("bind: %s", strerror(errno));
		close(sfd);
		return -1;
	}

	if (listen(sfd, 2) < 0) {
		msg("listen: %s", strerror(errno));
		close(sfd);
		return -1;
	}

	return sfd;
}

void
meter_init(const char *pathname)
{
	int sfd;
	sfd = meter_listen(pathname);
	if (sfd < 0) {
		msg("Could not set up meter socket; metering disabled");
		return;
	}

	meter_socket = sfd;
	onselect(meter_socket, NULL, meter_accept, NULL);
}
