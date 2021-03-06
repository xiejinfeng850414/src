/*	$OpenBSD: kroute.c,v 1.138 2017/08/14 22:12:59 krw Exp $	*/

/*
 * Copyright 2012 Kenneth R Westerback <krw@openbsd.org>
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

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <arpa/inet.h>

#include <net/if.h>
#include <net/if_types.h>
#include <net/route.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <imsg.h>
#include <limits.h>
#include <resolv.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dhcp.h"
#include "dhcpd.h"
#include "log.h"
#include "privsep.h"

#define ROUNDUP(a) \
	((a) > 0 ? (1 + (((a) - 1) | (sizeof(long) - 1))) : sizeof(long))

void	populate_rti_info(struct sockaddr **, struct rt_msghdr *);
void	add_route(struct in_addr, struct in_addr, struct in_addr, int);
void	flush_routes(void);
int	delete_addresses(char *, struct in_addr, struct in_addr);
char	*get_routes(int, size_t *);

char *
get_routes(int rdomain, size_t *len)
{
	int		 mib[7];
	char		*buf, *bufp, *errmsg = NULL;
	size_t		 needed;

	mib[0] = CTL_NET;
	mib[1] = PF_ROUTE;
	mib[2] = 0;
	mib[3] = AF_INET;
	mib[4] = NET_RT_FLAGS;
	mib[5] = RTF_STATIC | RTF_GATEWAY | RTF_LLINFO;
	mib[6] = rdomain;

	buf = NULL;
	errmsg = NULL;
	while (1) {
		if (sysctl(mib, 7, NULL, &needed, NULL, 0) == -1) {
			errmsg = "sysctl size of routes:";
			break;
		}
		if (needed == 0) {
			free(buf);
			return NULL;
		}
		if ((bufp = realloc(buf, needed)) == NULL) {
			errmsg = "routes buf realloc:";
			break;
		}
		buf = bufp;
		if (sysctl(mib, 7, buf, &needed, NULL, 0) == -1) {
			if (errno == ENOMEM)
				continue;
			errmsg = "sysctl retrieval of routes:";
			break;
		}
		break;
	}

	if (errmsg != NULL) {
		log_warn("get_routes - %s (msize=%zu)", errmsg, needed);
		free(buf);
		buf = NULL;
	}

	*len = needed;
	return buf;
}

/*
 * [priv_]flush_routes do the equivalent of
 *
 *	route -q -T $rdomain -n flush -inet -iface $interface
 *	arp -dan
 */
void
flush_routes(void)
{
	int	 rslt;

	rslt = imsg_compose(unpriv_ibuf, IMSG_FLUSH_ROUTES, 0, 0, -1, NULL, 0);
	if (rslt == -1)
		log_warn("flush_routes: imsg_compose");
}

void
priv_flush_routes(int index, int routefd, int rdomain)
{
	static int			 seqno;
	char				*lim, *buf = NULL, *next;
	struct rt_msghdr		*rtm;
	size_t				 len;
	ssize_t				 rlen;

	buf = get_routes(rdomain, &len);
	if (buf == NULL)
		return;

	lim = buf + len;
	for (next = buf; next < lim; next += rtm->rtm_msglen) {
		rtm = (struct rt_msghdr *)next;
		if (rtm->rtm_version != RTM_VERSION)
			continue;
		if (rtm->rtm_index != index)
			continue;
		if (rtm->rtm_tableid != rdomain)
			continue;
		if ((rtm->rtm_flags & (RTF_GATEWAY|RTF_STATIC|RTF_LLINFO)) == 0)
			continue;
		if ((rtm->rtm_flags & (RTF_LOCAL|RTF_BROADCAST)) != 0)
			continue;

		rtm->rtm_type = RTM_DELETE;
		rtm->rtm_seq = seqno++;

		rlen = write(routefd, (char *)rtm, rtm->rtm_msglen);
		if (rlen == -1) {
			if (errno != ESRCH)
				log_warn("RTM_DELETE write");
		} else if (rlen < (int)rtm->rtm_msglen)
			log_warnx("short RTM_DELETE write (%zd)\n", rlen);
	}

	free(buf);
}

void
set_routes(struct in_addr addr, struct in_addr addrmask, uint8_t *rtstatic,
    unsigned int rtstatic_len)
{
	const struct in_addr	 any = { INADDR_ANY };
	struct in_addr		 dest, gateway, netmask;
	unsigned int		 i, bits, bytes;

	flush_routes();

	/* Add classless static routes. */
	i = 0;
	while (i < rtstatic_len) {
		bits = rtstatic[i++];
		bytes = (bits + 7) / 8;

		if (bytes > sizeof(netmask.s_addr))
			return;
		else if (i + bytes > rtstatic_len)
			return;

		if (bits != 0)
			netmask.s_addr = htonl(0xffffffff << (32 - bits));
		else
			netmask.s_addr = INADDR_ANY;

		memcpy(&dest, &rtstatic[i], bytes);
		dest.s_addr = dest.s_addr & netmask.s_addr;
		i += bytes;

		if (i + sizeof(gateway) > rtstatic_len)
			return;
		memcpy(&gateway.s_addr, &rtstatic[i], sizeof(gateway.s_addr));
		i += sizeof(gateway.s_addr);

		if (gateway.s_addr == INADDR_ANY) {
			/*
			 * DIRECT ROUTE
			 *
			 * route add -net $dest -netmask $netmask -cloning
			 *     -iface $addr
			 */
			add_route(dest, netmask, addr,
			    RTF_STATIC | RTF_CLONING);
		} else if (netmask.s_addr == INADDR_ANY) {
			/*
			 * DEFAULT ROUTE
			 */
			if (addrmask.s_addr == INADDR_BROADCAST) {
				/*
				 * DIRECT ROUTE TO DEFAULT GATEWAY
				 *
				 * To be compatible with ISC DHCP behavior on
				 * Linux, if we were given a /32 IP assignment
				 * then add a /32 direct route for the gateway
				 * to make it routable.
				 *
				 * route add -net $gateway -netmask $addrmask
				 *     -cloning -iface $addr
				 */
				add_route(gateway, addrmask, addr,
				    RTF_STATIC | RTF_CLONING);
			}

			if (memcmp(&gateway, &addr, sizeof(addr)) == 0) {
				/*
				 * DEFAULT ROUTE IS A DIRECT ROUTE
				 *
				 * route add default -iface $addr
				 */
				add_route(any, any, gateway, RTF_STATIC);
			} else {
				/*
				 * DEFAULT ROUTE IS VIA GATEWAY
				 *
				 * route add default $gateway
				 */
				add_route(any, any, gateway,
				    RTF_STATIC | RTF_GATEWAY);
			}
		} else {
			/*
			 * NON-DIRECT, NON-DEFAULT ROUTE
			 *
			 * route add -net $dest -netmask $netmask $gateway
			 */
			add_route(dest, netmask, gateway,
			    RTF_STATIC | RTF_GATEWAY);
		}
	}
}

/*
 * [priv_]add_route() add a single route to the routing table.
 */
void
add_route(struct in_addr dest, struct in_addr netmask, struct in_addr gateway,
    int flags)
{
	struct imsg_add_route	 imsg;
	int			 rslt;

	imsg.dest = dest;
	imsg.gateway = gateway;
	imsg.netmask = netmask;
	imsg.flags = flags;

	rslt = imsg_compose(unpriv_ibuf, IMSG_ADD_ROUTE, 0, 0, -1,
	    &imsg, sizeof(imsg));
	if (rslt == -1)
		log_warn("add_route: imsg_compose");
}

void
priv_add_route(char *name, int rdomain, int routefd,
    struct imsg_add_route *imsg)
{
	char			 destbuf[INET_ADDRSTRLEN];
	char			 maskbuf[INET_ADDRSTRLEN];
	struct iovec		 iov[5];
	struct rt_msghdr	 rtm;
	struct sockaddr_in	 dest, gateway, mask;
	int			 i, index, iovcnt = 0;

	index = if_nametoindex(name);
	if (index == 0)
		return;

	/* Build RTM header */

	memset(&rtm, 0, sizeof(rtm));

	rtm.rtm_version = RTM_VERSION;
	rtm.rtm_type = RTM_ADD;
	rtm.rtm_index = index;
	rtm.rtm_tableid = rdomain;
	rtm.rtm_priority = RTP_NONE;
	rtm.rtm_addrs =	RTA_DST | RTA_NETMASK | RTA_GATEWAY;
	rtm.rtm_flags = imsg->flags;

	rtm.rtm_msglen = sizeof(rtm);
	iov[iovcnt].iov_base = &rtm;
	iov[iovcnt++].iov_len = sizeof(rtm);

	/* Add the destination address. */
	memset(&dest, 0, sizeof(dest));
	dest.sin_len = sizeof(dest);
	dest.sin_family = AF_INET;
	dest.sin_addr.s_addr = imsg->dest.s_addr;

	rtm.rtm_msglen += sizeof(dest);
	iov[iovcnt].iov_base = &dest;
	iov[iovcnt++].iov_len = sizeof(dest);

	/* Add the gateways address. */
	memset(&gateway, 0, sizeof(gateway));
	gateway.sin_len = sizeof(gateway);
	gateway.sin_family = AF_INET;
	gateway.sin_addr.s_addr = imsg->gateway.s_addr;

	rtm.rtm_msglen += sizeof(gateway);
	iov[iovcnt].iov_base = &gateway;
	iov[iovcnt++].iov_len = sizeof(gateway);

	/* Add the network mask. */
	memset(&mask, 0, sizeof(mask));
	mask.sin_len = sizeof(mask);
	mask.sin_family = AF_INET;
	mask.sin_addr.s_addr = imsg->netmask.s_addr;

	rtm.rtm_msglen += sizeof(mask);
	iov[iovcnt].iov_base = &mask;
	iov[iovcnt++].iov_len = sizeof(mask);

	/* Check for EEXIST since other dhclient may not be done. */
	for (i = 0; i < 5; i++) {
		if (writev(routefd, iov, iovcnt) != -1)
			break;
		if (i == 4) {
			strlcpy(destbuf, inet_ntoa(imsg->dest),
			    sizeof(destbuf));
			strlcpy(maskbuf, inet_ntoa(imsg->netmask),
			    sizeof(maskbuf));
			log_warn("failed to add route (%s/%s via %s)",
			    destbuf, maskbuf, inet_ntoa(imsg->gateway));
		} else if (errno == EEXIST || errno == ENETUNREACH)
			sleep(1);
	}
}

/*
 * delete_addresses() deletes existing inet addresses on the named interface,
 * leaving in place newaddr/newnetmask.
 *
 * Return 1 if newaddr/newnetmask is seen while deleting addresses, 0 otherwise.
 */
int
delete_addresses(char *name, struct in_addr newaddr, struct in_addr newnetmask)
{
	struct in_addr		 addr, netmask;
	struct ifaddrs		*ifap, *ifa;
	int			 found = 0;

	if (getifaddrs(&ifap) != 0)
		fatal("delete_addresses getifaddrs");

	for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
		if ((ifa->ifa_flags & IFF_LOOPBACK) != 0 ||
		    (ifa->ifa_flags & IFF_POINTOPOINT) != 0 ||
		    ((ifa->ifa_flags & IFF_UP) == 0) ||
		    (ifa->ifa_addr->sa_family != AF_INET) ||
		    (strcmp(name, ifa->ifa_name) != 0))
			continue;

		memcpy(&addr,
		    &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
		    sizeof(addr));
		memcpy(&netmask,
		    &((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr,
		    sizeof(netmask));

		if (addr.s_addr == newaddr.s_addr &&
		    netmask.s_addr == newnetmask.s_addr)
			found = 1;
		else
			delete_address(addr);
	}

	freeifaddrs(ifap);
	return (found);
}

/*
 * [priv_]delete_address is the equivalent of
 *
 *	ifconfig <ifname> inet <addr> delete
 */
void
delete_address(struct in_addr addr)
{
	struct imsg_delete_address	 imsg;
	int				 rslt;

	imsg.addr = addr;

	rslt = imsg_compose(unpriv_ibuf, IMSG_DELETE_ADDRESS, 0, 0 , -1, &imsg,
	    sizeof(imsg));
	if (rslt == -1)
		log_warn("delete_address: imsg_compose");
}

void
priv_delete_address(char *name, int ioctlfd, struct imsg_delete_address *imsg)
{
	struct ifaliasreq	 ifaliasreq;
	struct sockaddr_in	*in;

	/*
	 * Delete specified address on specified interface.
	 */

	memset(&ifaliasreq, 0, sizeof(ifaliasreq));
	strncpy(ifaliasreq.ifra_name, name, sizeof(ifaliasreq.ifra_name));

	in = (struct sockaddr_in *)&ifaliasreq.ifra_addr;
	in->sin_family = AF_INET;
	in->sin_len = sizeof(ifaliasreq.ifra_addr);
	in->sin_addr.s_addr = imsg->addr.s_addr;

	/* SIOCDIFADDR will result in a RTM_DELADDR message we must catch! */
	if (ioctl(ioctlfd, SIOCDIFADDR, &ifaliasreq) == -1) {
		if (errno != EADDRNOTAVAIL)
			log_warn("SIOCDIFADDR failed (%s)",
			    inet_ntoa(imsg->addr));
	}
}

/*
 * [priv_]set_mtu is the equivalent of
 *
 *      ifconfig <if> mtu <mtu>
 */
void
set_mtu(int inits, uint16_t mtu)
{
	struct imsg_set_mtu	 imsg;
	int			 rslt;

	if ((inits & RTV_MTU) == 0)
		return;

	if (mtu < 68) {
		log_warnx("mtu size %u < 68: ignored", mtu);
		return;
	}
	imsg.mtu = mtu;

	rslt = imsg_compose(unpriv_ibuf, IMSG_SET_MTU, 0, 0, -1,
	    &imsg, sizeof(imsg));
	if (rslt == -1)
		log_warn("set_mtu: imsg_compose");
}

void
priv_set_mtu(char *name, int ioctlfd, struct imsg_set_mtu *imsg)
{
	struct ifreq	 ifr;

	memset(&ifr, 0, sizeof(ifr));

	strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));
	ifr.ifr_mtu = imsg->mtu;

	if (ioctl(ioctlfd, SIOCSIFMTU, &ifr) == -1)
		log_warn("SIOCSIFMTU failed (%d)", imsg->mtu);
}

/*
 * [priv_]set_address is the equivalent of
 *
 *	ifconfig <if> inet <addr> netmask <mask> broadcast <addr>
 */
void
set_address(char *name, struct in_addr addr, struct in_addr netmask)
{
	struct imsg_set_address	 imsg;
	int			 rslt;

	/* Deleting the addresses also clears out arp entries. */
	if (delete_addresses(name, addr, netmask) != 0)
		return;

	imsg.addr = addr;
	imsg.mask = netmask;

	rslt = imsg_compose(unpriv_ibuf, IMSG_SET_ADDRESS, 0, 0, -1, &imsg,
	    sizeof(imsg));
	if (rslt == -1)
		log_warn("set_address: imsg_compose");
}

void
priv_set_address(char *name, int ioctlfd, struct imsg_set_address *imsg)
{
	struct ifaliasreq	 ifaliasreq;
	struct sockaddr_in	*in;

	memset(&ifaliasreq, 0, sizeof(ifaliasreq));
	strncpy(ifaliasreq.ifra_name, name, sizeof(ifaliasreq.ifra_name));

	/* The actual address in ifra_addr. */
	in = (struct sockaddr_in *)&ifaliasreq.ifra_addr;
	in->sin_family = AF_INET;
	in->sin_len = sizeof(ifaliasreq.ifra_addr);
	in->sin_addr.s_addr = imsg->addr.s_addr;

	/* And the netmask in ifra_mask. */
	in = (struct sockaddr_in *)&ifaliasreq.ifra_mask;
	in->sin_family = AF_INET;
	in->sin_len = sizeof(ifaliasreq.ifra_mask);
	memcpy(&in->sin_addr, &imsg->mask, sizeof(in->sin_addr));

	/* No need to set broadcast address. Kernel can figure it out. */

	if (ioctl(ioctlfd, SIOCAIFADDR, &ifaliasreq) == -1)
		log_warn("SIOCAIFADDR failed (%s)", inet_ntoa(imsg->addr));
}

/*
 * [priv_]write_resolv_conf write out a new resolv.conf.
 */
void
write_resolv_conf(void)
{
	int	 rslt;

	rslt = imsg_compose(unpriv_ibuf, IMSG_WRITE_RESOLV_CONF,
	    0, 0, -1, NULL, 0);
	if (rslt == -1)
		log_warn("write_resolv_conf: imsg_compose");
}

void
priv_write_resolv_conf(char *contents)
{
	const char	*path = "/etc/resolv.conf";
	ssize_t		 n;
	size_t		 sz;
	int		 fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC,
	    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	if (fd == -1) {
		log_warn("Couldn't open '%s'", path);
		return;
	}

	if (contents != NULL) {
		sz = strlen(contents);
		n = write(fd, contents, sz);
		if (n == -1)
			log_warn("Couldn't write contents to '%s'", path);
		else if ((size_t)n < sz)
			log_warnx("Short contents write to '%s' (%zd vs %zu)",
			    path, n, sz);
	}

	close(fd);
}

/*
 * default_route_index returns the index of the interface which the
 * default route is on.
 */
int
default_route_index(int rdomain, int routefd)
{
	struct iovec		 iov[3];
	struct sockaddr_in	 sin;
	struct {
		struct rt_msghdr	m_rtm;
		char			m_space[512];
	} m_rtmsg;
	pid_t			 pid;
	ssize_t			 len;
	int			 seq, iovcnt = 0;

	/* Build RTM header */

	memset(&m_rtmsg, 0, sizeof(m_rtmsg));

	m_rtmsg.m_rtm.rtm_version = RTM_VERSION;
	m_rtmsg.m_rtm.rtm_type = RTM_GET;
	m_rtmsg.m_rtm.rtm_seq = seq = arc4random();
	m_rtmsg.m_rtm.rtm_tableid = rdomain;
	m_rtmsg.m_rtm.rtm_addrs = RTA_DST | RTA_NETMASK;

	iov[iovcnt].iov_base = &m_rtmsg.m_rtm;
	iov[iovcnt++].iov_len = sizeof(m_rtmsg.m_rtm);

	/* Ask for route to 0.0.0.0/0 (a.k.a. the default route). */
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;

	iov[iovcnt].iov_base = &sin;
	iov[iovcnt++].iov_len = sizeof(sin);
	iov[iovcnt].iov_base = &sin;
	iov[iovcnt++].iov_len = sizeof(sin);

	m_rtmsg.m_rtm.rtm_msglen = sizeof(m_rtmsg.m_rtm) + 2 * sizeof(sin);

	if (writev(routefd, iov, iovcnt) == -1) {
		if (errno != ESRCH)
			log_warn("RTM_GET of default route");
		goto done;
	}

	pid = getpid();

	do {
		len = read(routefd, &m_rtmsg, sizeof(m_rtmsg));
		if (len == -1) {
			log_warn("get default route read");
			goto done;
		} else if (len == 0) {
			log_warnx("no data from default route read");
			goto done;
		}
		if (m_rtmsg.m_rtm.rtm_version == RTM_VERSION &&
		    m_rtmsg.m_rtm.rtm_type == RTM_GET &&
		    m_rtmsg.m_rtm.rtm_pid == pid &&
		    m_rtmsg.m_rtm.rtm_seq == seq) {
			if (m_rtmsg.m_rtm.rtm_errno != 0) {
				log_warnx("default route read rtm: %s",
				    strerror(m_rtmsg.m_rtm.rtm_errno));
				goto done;
			}
			return m_rtmsg.m_rtm.rtm_index;
		}
	} while (1);

done:
	return 0;
}

/*
 * set_resolv_conf creates a string that are the resolv.conf contents
 * that should be used when the interface is determined to be the one to
 * create /etc/resolv.conf
 */
void
set_resolv_conf(char *name, uint8_t *rtsearch, unsigned int rtsearch_len,
    uint8_t *rtdns, unsigned int rtdns_len)
{
	char		*dn, *nss[MAXNS], *contents, *courtesy;
	struct in_addr	*addr;
	size_t		 len;
	unsigned int	 i, servers;
	int		 rslt;

	memset(nss, 0, sizeof(nss));
	len = 0;

	if (rtsearch_len != 0) {
		rslt = asprintf(&dn, "search %.*s\n", rtsearch_len,
		    rtsearch);
		if (rslt == -1)
			dn = NULL;
	} else
		dn = strdup("");
	if (dn == NULL)
		fatalx("no memory for domainname");
	len += strlen(dn);

	if (rtdns_len != 0) {
		addr = (struct in_addr *)rtdns;
		servers = rtdns_len / sizeof(addr->s_addr);
		if (servers > MAXNS)
			servers = MAXNS;
		for (i = 0; i < servers; i++) {
			rslt = asprintf(&nss[i], "nameserver %s\n",
			    inet_ntoa(*addr));
			if (rslt == -1)
				fatalx("no memory for nameserver");
			len += strlen(nss[i]);
			addr++;
		}
	}

	/*
	 * XXX historically dhclient-script did not overwrite
	 *     resolv.conf when neither search nor dns info
	 *     was provided. Is that really what we want?
	 */
	if (len > 0 && config->resolv_tail != NULL)
		len += strlen(config->resolv_tail);

	if (len == 0) {
		free(dn);
		return;
	}

	rslt = asprintf(&courtesy, "# Generated by %s dhclient\n", name);
	if (rslt == -1)
		fatalx("no memory for courtesy line");
	len += strlen(courtesy);

	len++; /* Need room for terminating NUL. */
	contents = calloc(1, len);
	if (contents == NULL)
		fatalx("no memory for resolv.conf contents");

	strlcat(contents, courtesy, len);
	free(courtesy);

	strlcat(contents, dn, len);
	free(dn);

	for (i = 0; i < MAXNS; i++) {
		if (nss[i] != NULL) {
			strlcat(contents, nss[i], len);
			free(nss[i]);
		}
	}

	if (config->resolv_tail != NULL)
		strlcat(contents, config->resolv_tail, len);

	rslt = imsg_compose(unpriv_ibuf, IMSG_SET_RESOLV_CONF,
	    0, 0, -1, contents, len);
	if (rslt == -1)
		log_warn("set_resolv_conf: imsg_compose");
}

/*
 * populate_rti_info populates the rti_info with pointers to the
 * sockaddr's contained in a rtm message.
 */
void
populate_rti_info(struct sockaddr **rti_info, struct rt_msghdr *rtm)
{
	struct sockaddr	*sa;
	int		 i;

	sa = (struct sockaddr *)((char *)(rtm) + rtm->rtm_hdrlen);

	for (i = 0; i < RTAX_MAX; i++) {
		if (rtm->rtm_addrs & (1 << i)) {
			rti_info[i] = sa;
			sa = (struct sockaddr *)((char *)(sa) +
			    ROUNDUP(sa->sa_len));
		} else
			rti_info[i] = NULL;
	}
}
