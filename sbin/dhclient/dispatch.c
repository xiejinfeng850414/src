/*	$OpenBSD: dispatch.c,v 1.139 2017/08/13 17:57:32 krw Exp $	*/

/*
 * Copyright 2004 Henning Brauer <henning@openbsd.org>
 * Copyright (c) 1995, 1996, 1997, 1998, 1999
 * The Internet Software Consortium.   All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of The Internet Software Consortium nor the names
 *    of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INTERNET SOFTWARE CONSORTIUM AND
 * CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE INTERNET SOFTWARE CONSORTIUM OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This software has been written for the Internet Software Consortium
 * by Ted Lemon <mellon@fugue.com> in cooperation with Vixie
 * Enterprises.  To learn more about the Internet Software Consortium,
 * see ``http://www.vix.com/isc''.  To learn more about Vixie
 * Enterprises, see ``http://www.vix.com''.
 */

#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_media.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <arpa/inet.h>

#include <errno.h>
#include <ifaddrs.h>
#include <imsg.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dhcp.h"
#include "dhcpd.h"
#include "log.h"
#include "privsep.h"


void packethandler(struct interface_info *ifi);
void flush_unpriv_ibuf(const char *);

/*
 * Loop waiting for packets, timeouts or routing messages.
 */
void
dispatch(struct interface_info *ifi, int routefd)
{
	struct pollfd		 fds[3];
	void			(*func)(struct interface_info *);
	time_t			 cur_time, howlong;
	int			 nfds, to_msec;

	while (quit == 0 || quit == SIGHUP) {
		if (quit == SIGHUP) {
			sendhup();
			to_msec = 100;
		} else if (ifi->timeout_func != NULL) {
			time(&cur_time);
			if (ifi->timeout <= cur_time) {
				func = ifi->timeout_func;
				cancel_timeout(ifi);
				(*(func))(ifi);
				continue;
			}
			/*
			 * Figure timeout in milliseconds, and check for
			 * potential overflow, so we can cram into an
			 * int for poll, while not polling with a
			 * negative timeout and blocking indefinitely.
			 */
			howlong = ifi->timeout - cur_time;
			if (howlong > INT_MAX / 1000)
					howlong = INT_MAX / 1000;
				to_msec = howlong * 1000;
		} else
			to_msec = -1;

		/*
		 * Set up the descriptors to be polled.
		 *
		 *  fds[0] == bpf socket for incoming packets
		 *  fds[1] == routing socket for incoming RTM messages
		 *  fds[2] == imsg socket to privileged process
		 */
		fds[0].fd = ifi->bfdesc;
		fds[1].fd = routefd;
		fds[2].fd = unpriv_ibuf->fd;
		fds[0].events = fds[1].events = fds[2].events = POLLIN;

		if (unpriv_ibuf->w.queued)
			fds[2].events |= POLLOUT;

		nfds = poll(fds, 3, to_msec);
		if (nfds == -1) {
			if (errno == EINTR)
				continue;
			log_warn("dispatch poll");
			quit = INTERNALSIG;
			continue;
		}

		if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			log_warnx("bfdesc poll error");
			quit = INTERNALSIG;
			continue;
		}
		if ((fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			log_warnx("routefd poll error");
			quit = INTERNALSIG;
			continue;
		}
		if ((fds[2].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			log_warnx("unpriv_ibuf poll error");
			quit = INTERNALSIG;
			continue;
		}

		if (nfds == 0)
			continue;

		if ((fds[0].revents & POLLIN) != 0) {
			do {
				packethandler(ifi);
			} while (ifi->rbuf_offset < ifi->rbuf_len);
		}
		if ((fds[1].revents & POLLIN) != 0)
			routehandler(ifi, routefd);
		if ((fds[2].revents & POLLOUT) != 0)
			flush_unpriv_ibuf("dispatch");
		if ((fds[2].revents & POLLIN) != 0)
			quit = INTERNALSIG;
	}

	if (quit != INTERNALSIG && quit != SIGHUP)
		fatalx("%s", strsignal(quit));
}

void
packethandler(struct interface_info *ifi)
{
	struct sockaddr_in	 from;
	struct ether_addr	 hfrom;
	struct in_addr		 ifrom;
	struct dhcp_packet	*packet = &ifi->recv_packet;
	struct reject_elem	*ap;
	struct option_data	*options;
	char			*type, *info;
	ssize_t			 result;
	void			(*handler)(struct interface_info *,
	    struct option_data *, char *);
	int			 i, rslt;

	if ((result = receive_packet(ifi, &from, &hfrom)) == -1) {
		ifi->errors++;
		if (ifi->errors > 20)
			fatalx("%s too many receive_packet failures",
			    ifi->name);
		else
			log_warn("%s receive_packet failed", ifi->name);
		return;
	}
	ifi->errors = 0;

	if (result == 0)
		return;

	ifrom.s_addr = from.sin_addr.s_addr;

	if (packet->hlen != ETHER_ADDR_LEN) {
#ifdef DEBUG
		log_debug("Discarding packet with hlen != %s (%u)",
		    ifi->name, packet->hlen);
#endif	/* DEBUG */
		return;
	} else if (memcmp(&ifi->hw_address, packet->chaddr,
	    sizeof(ifi->hw_address))) {
#ifdef DEBUG
		log_debug("Discarding packet with chaddr != %s (%s)",
		    ifi->name,
		    ether_ntoa((struct ether_addr *)packet->chaddr));
#endif	/* DEBUG */
		return;
	}

	if (ifi->xid != packet->xid) {
#ifdef DEBUG
		log_debug("Discarding packet with XID != %u (%u)", ifi->xid,
		    packet->xid);
#endif	/* DEBUG */
		return;
	}

	TAILQ_FOREACH(ap, &config->reject_list, next)
	    if (ifrom.s_addr == ap->addr.s_addr) {
#ifdef DEBUG
		    log_debug("Discarding packet from address on reject "
			"list (%s)", inet_ntoa(ifrom));
#endif	/* DEBUG */
		    return;
	    }

	options = unpack_options(&ifi->recv_packet);

	/*
	 * RFC 6842 says if the server sends a client identifier
	 * that doesn't match then the packet must be dropped.
	 */
	i = DHO_DHCP_CLIENT_IDENTIFIER;
	if ((options[i].len != 0) &&
	    ((options[i].len != config->send_options[i].len) ||
	    memcmp(options[i].data, config->send_options[i].data,
	    options[i].len) != 0)) {
#ifdef DEBUG
		log_debug("Discarding packet with client-identifier "
		    "'%s'", pretty_print_option(i, &options[i], 0));
#endif	/* DEBUG */
		return;
	}

	type = "<unknown>";
	handler = NULL;

	i = DHO_DHCP_MESSAGE_TYPE;
	if (options[i].data != NULL) {
		/* Always try a DHCP packet, even if a bad option was seen. */
		switch (options[i].data[0]) {
		case DHCPOFFER:
			handler = dhcpoffer;
			type = "DHCPOFFER";
			break;
		case DHCPNAK:
			handler = dhcpnak;
			type = "DHCPNACK";
			break;
		case DHCPACK:
			handler = dhcpack;
			type = "DHCPACK";
			break;
		default:
#ifdef DEBUG
			log_debug("Discarding DHCP packet of unknown type "
			    "(%d)", options[i].data[0]);
#endif	/* DEBUG */
			return;
		}
	} else if (packet->op == BOOTREPLY) {
		handler = dhcpoffer;
		type = "BOOTREPLY";
	} else {
#ifdef DEBUG
		log_debug("Discarding packet which is neither DHCP nor BOOTP");
#endif	/* DEBUG */
		return;
	}

	rslt = asprintf(&info, "%s from %s (%s)", type, inet_ntoa(ifrom),
	    ether_ntoa(&hfrom));
	if (rslt == -1)
		fatalx("no memory for info string");

	if (handler != NULL)
		(*handler)(ifi, options, info);

	free(info);
}

/*
 * flush_unpriv_ibuf stuffs queued messages into the imsg socket.
 */
void
flush_unpriv_ibuf(const char *who)
{
	while (unpriv_ibuf->w.queued) {
		if (msgbuf_write(&unpriv_ibuf->w) <= 0) {
			if (errno == EAGAIN)
				break;
			if (quit == 0)
				quit = INTERNALSIG;
			if (errno != EPIPE && errno != 0)
				log_warn("%s: msgbuf_write", who);
			break;
		}
	}
}

void
set_timeout(struct interface_info *ifi, time_t secs,
    void (*where)(struct interface_info *))
{
	time(&ifi->timeout);
	ifi->timeout += secs;
	ifi->timeout_func = where;
}

void
cancel_timeout(struct interface_info *ifi)
{
	ifi->timeout = 0;
	ifi->timeout_func = NULL;
}

/*
 * Inform the [priv] process a HUP was received.
 */
void
sendhup(void)
{
	int rslt;

	rslt = imsg_compose(unpriv_ibuf, IMSG_HUP, 0, 0, -1, NULL, 0);
	if (rslt == -1)
		log_warn("sendhup: imsg_compose");
}
