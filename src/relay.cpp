/*
 * relay.cpp - Miredo: binding between libtun6 and libteredo
 * $Id$
 */

/***********************************************************************
 *  Copyright © 2004-2006 Rémi Denis-Courmont.                         *
 *  This program is free software; you can redistribute and/or modify  *
 *  it under the terms of the GNU General Public License as published  *
 *  by the Free Software Foundation; version 2 of the license.         *
 *                                                                     *
 *  This program is distributed in the hope that it will be useful,    *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of     *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.               *
 *  See the GNU General Public License for more details.               *
 *                                                                     *
 *  You should have received a copy of the GNU General Public License  *
 *  along with this program; if not, you can get it from:              *
 *  http://www.gnu.org/copyleft/gpl.html                               *
 ***********************************************************************/

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <gettext.h>

#if HAVE_STDINT_H
# include <stdint.h>
#elif HAVE_INTTYPES_H
# include <inttypes.h>
#endif

#include <stdlib.h> // free()
#include <sys/types.h>
#include <string.h> // strerror()
#include <errno.h>
#include <unistd.h> // close()
#include <sys/wait.h> // wait()
#include <sys/select.h> // pselect()
#include <signal.h> // sigemptyset()
#include <fcntl.h> // fcntl()
#include <compat/pselect.h>
#include <syslog.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <arpa/inet.h> // inet_ntop()
#include <netdb.h> // NI_MAXHOST
#ifdef HAVE_SYS_CAPABILITY_H
# include <sys/capability.h>
#endif
#ifndef SOL_IPV6
# define SOL_IPV6 IPPROTO_IPV6
#endif
#ifndef SOL_ICMPV6
# define SOL_ICMPV6 IPPROTO_ICMPV6
#endif

#include <libtun6/tun6.h>

#include <libteredo/teredo.h>
#include <libteredo/tunnel.h>

#include "privproc.h"
#include "addrwatch.h"
#include "conf.h"
#include "miredo.h"

const char *const miredo_name = "miredo";
const char *const miredo_pidfile = LOCALSTATEDIR"/run/miredo.pid";

#ifdef HAVE_LIBCAP
static const cap_value_t capv[] =
{
	CAP_KILL, /* required by the signal handler */
	CAP_SETUID,
	CAP_SYS_CHROOT,
	CAP_NET_ADMIN, /* required by libtun6 */
	CAP_NET_RAW /* required for raw ICMPv6 socket */
};

const cap_value_t *miredo_capv = capv;
const int miredo_capc = sizeof (capv) / sizeof (capv[0]);
#endif


extern "C" int
miredo_diagnose (void)
{
	char errbuf[LIBTUN6_ERRBUF_SIZE];
	if (tun6_driver_diagnose (errbuf))
	{
		fputs (errbuf, stderr);
		return -1;
	}
	
	return 0;
}


typedef struct miredo_tunnel
{
	const tun6 *tunnel;
	int priv_fd;
} miredo_tunnel;

static int icmp6_fd = -1;

static int miredo_init (bool client)
{
	if (teredo_startup (client))
		return -1;

	assert (icmp6_fd == -1);

	struct icmp6_filter filt;

	icmp6_fd = socket (AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (icmp6_fd == -1)
		return -1;

	fcntl (icmp6_fd, F_SETFD, FD_CLOEXEC);
	int val = fcntl (icmp6_fd, F_GETFL);
	if (val == -1)
		val = 0;
	fcntl (icmp6_fd, F_SETFL, O_NONBLOCK | val);

	val = 2;
	setsockopt (icmp6_fd, SOL_IPV6, IPV6_CHECKSUM, &val, sizeof (val));

	/* We don't use the socket for receive -> block all */
	ICMP6_FILTER_SETBLOCKALL (&filt);
	setsockopt (icmp6_fd, SOL_ICMPV6, ICMP6_FILTER, &filt, sizeof (filt));
	return 0;
}


static void miredo_deinit (bool client)
{
	assert (icmp6_fd != -1);
	close (icmp6_fd);
	teredo_cleanup (client);
}


/**
 * Callback to transmit decapsulated Teredo IPv6 packets to the kernel.
 */
static void
miredo_recv_callback (void *data, const void *packet, size_t length)
{
	assert (data != NULL);

	(void)tun6_send (((miredo_tunnel *)data)->tunnel, packet, length);
}


/**
 * Callback to emit an ICMPv6 error message through a raw ICMPv6 socket.
 */
static void
miredo_icmp6_callback (void *, const void *packet, size_t length,
                        const struct in6_addr *dst)
{
	assert (icmp6_fd != -1);

	struct sockaddr_in6 addr;
	memset (&addr, 0, sizeof (addr));
	/* TODO: use sendmsg and don't memcpy in BuildICMPv6Error */
	addr.sin6_family = AF_INET6;
#ifdef HAVE_SA_LEN
	addr.sin6_len = sizeof (addr);
#endif
	memcpy (&addr.sin6_addr, dst, sizeof (addr.sin6_addr));
	(void)sendto (icmp6_fd, packet, length, 0,
	              (struct sockaddr *)&addr, sizeof (addr));
}


#define TEREDO_CONE     0
#define TEREDO_RESTRICT 1
#define TEREDO_CLIENT   2
#define TEREDO_EXCLIENT 3

static bool
ParseRelayType (MiredoConf& conf, const char *name, int *type)
{
	unsigned line;
	char *val = conf.GetRawValue (name, &line);

	if (val == NULL)
		return true;

	if (strcasecmp (val, "client") == 0)
		*type = TEREDO_CLIENT;
	else if (strcasecmp (val, "autoclient") == 0)
		*type = TEREDO_EXCLIENT;
	else if (strcasecmp (val, "cone") == 0)
		*type = TEREDO_CONE;
	else if (strcasecmp (val, "restricted") == 0)
		*type = TEREDO_RESTRICT;
	else
	{
		syslog (LOG_ERR, _("Invalid relay type \"%s\" at line %u"),
		        val, line);
		free (val);
		return false;
	}
	free (val);
	return true;
}


#ifdef MIREDO_TEREDO_CLIENT
static tun6 *
create_dynamic_tunnel (const char *ifname, int *fd)
{
	tun6 *tunnel = tun6_create (ifname);

	if (tunnel == NULL)
		return NULL;

	/* FIXME: we leak all heap-allocated settings in the child process */
	int res = miredo_privileged_process (tunnel);
	if (res == -1)
	{
		tun6_destroy (tunnel);
		return NULL;
	}
	*fd = res;
	return tunnel;
}


/**
 * Callback to configure a Teredo tunneling interface.
 */
static void
miredo_up_callback (void *data, const struct in6_addr *addr, uint16_t mtu)
{
	char str[INET6_ADDRSTRLEN];

	syslog (LOG_NOTICE, _("Teredo pseudo-tunnel started"));
	if (inet_ntop (AF_INET6, addr, str, sizeof (str)) != NULL)
		syslog (LOG_INFO, _(" (address: %s, MTU: %u)"),
				str, (unsigned)mtu);

	assert (data != NULL);

	miredo_configure_tunnel (((miredo_tunnel *)data)->priv_fd, addr, mtu);
}


/**
 * Callback to deconfigure a Teredo tunneling interface.
 */
static void
miredo_down_callback (void *data)
{
	assert (data != NULL);

	miredo_configure_tunnel (((miredo_tunnel *)data)->priv_fd, &in6addr_any,
	                         1280);
	syslog (LOG_NOTICE, _("Teredo pseudo-tunnel stopped"));
}


static int
setup_client (teredo_tunnel *client, const char *server, const char *server2)
{
	teredo_set_state_cb (client, miredo_up_callback, miredo_down_callback);
	return teredo_set_client_mode (client, server, server2);
}

static inline const struct timespec *watch_ts (const miredo_addrwatch *watch)
{
	static const struct timespec watch_ts = { 5, 0 };
	return (watch != NULL) ? &watch_ts : NULL;
}
#else
# define create_dynamic_tunnel( a, b )   NULL
# define setup_client( a, b, c )         (-1)
# define miredo_addrwatch_available( a ) 0
# define watch_ts( a )                   NULL
# define run_tunnel( a, b, c )           run_tunnel_RELAY_ONLY( a, b )
#endif


static tun6 *
create_static_tunnel (const char *ifname, const struct in6_addr *prefix,
                      uint16_t mtu, bool cone)
{
	tun6 *tunnel = tun6_create (ifname);

	if (tunnel == NULL)
		return NULL;

	if (tun6_setMTU (tunnel, mtu) || tun6_bringUp (tunnel)
	 || tun6_addAddress (tunnel, cone ? &teredo_cone : &teredo_restrict, 64)
	 || tun6_addRoute (tunnel, prefix, 32, 0))
	{
		tun6_destroy (tunnel);
		return NULL;
	}
	return tunnel;
}



static int
setup_relay (teredo_tunnel *relay, uint32_t prefix, bool cone)
{
	teredo_set_prefix (relay, prefix);
	return teredo_set_cone_flag (relay, cone);
}


/**
 * Miredo main deamon function, with UDP datagrams and IPv6 packets
 * receive loop.
 */
static int
run_tunnel (teredo_tunnel *relay, tun6 *tunnel, miredo_addrwatch *w)
{
	fd_set refset;

	FD_ZERO (&refset);
	int maxfd = tun6_registerReadSet (tunnel, &refset);

	int val = teredo_register_readset (relay, &refset);
	if (val > maxfd)
		maxfd = val;

	maxfd++;

	sigset_t sigset;
	sigemptyset (&sigset);

	/* Main loop */
	while (!miredo_addrwatch_available (w))
	{
		fd_set readset;
		memcpy (&readset, &refset, sizeof (readset));

		/* Wait until one of them is ready for read */
		val = pselect (maxfd, &readset, NULL, NULL, watch_ts (w), &sigset);
		if (val < 0)
			return 0;
		if (val == 0)
			continue;

		/* Handle incoming data */
		union
		{
			struct ip6_hdr ip6;
			uint8_t fill[65507];
		} pbuf;

		/* Forwards IPv6 packet to Teredo
		* (Packet transmission) */
		val = tun6_recv (tunnel, &readset, &pbuf.ip6, sizeof (pbuf));
		if (val >= 40)
			teredo_transmit (relay, &pbuf.ip6, val);

		/* Forwards Teredo packet to IPv6
		* (Packet reception) */
		teredo_run (relay);
	}
	return -2;
}


extern int
miredo_run (MiredoConf& conf, const char *server_name)
{
	/*
	 * CONFIGURATION
	 */
	union teredo_addr prefix;
	memset (&prefix, 0, sizeof (prefix));
	prefix.teredo.prefix = htonl (TEREDO_PREFIX);

	int mode = TEREDO_CLIENT;
	if (!ParseRelayType (conf, "RelayType", &mode))
	{
		syslog (LOG_ALERT, _("Fatal configuration error"));
		return -2;
	}

#ifdef MIREDO_TEREDO_CLIENT
	const char *server_name2 = NULL;
	char namebuf[NI_MAXHOST], namebuf2[NI_MAXHOST];
#endif
	uint16_t mtu = 1280;

	if (mode & TEREDO_CLIENT)
	{
#ifdef MIREDO_TEREDO_CLIENT
		if (server_name == NULL)
		{
			char *name = conf.GetRawValue ("ServerAddress");
			if (name == NULL)
			{
				syslog (LOG_ALERT, _("Server address not specified"));
				syslog (LOG_ALERT, _("Fatal configuration error"));
				return -2;
			}
			strlcpy (namebuf, name, sizeof (namebuf));
			free (name);
			server_name = namebuf;

			name = conf.GetRawValue ("ServerAddress2");
			if (name != NULL)
			{
				strlcpy (namebuf2, name, sizeof (namebuf2));
				free (name);
				server_name2 = namebuf2;
			}
		}
#else
		syslog (LOG_ALERT, _("Unsupported Teredo client mode"));
		syslog (LOG_ALERT, _("Fatal configuration error"));
		return -2;
#endif
	}
	else
	{
		server_name = NULL;
		mtu = 1280;

		if (!ParseIPv6 (conf, "Prefix", &prefix.ip6)
		 || !conf.GetInt16 ("InterfaceMTU", &mtu))
		{
			syslog (LOG_ALERT, _("Fatal configuration error"));
			return -2;
		}
	}

	uint32_t bind_ip = INADDR_ANY;
	uint16_t bind_port = 
#if 0
		/*
		 * We use 3545 as a Teredo service port.
		 * It is better to use a fixed port number for the
		 * purpose of firewalling, rather than a pseudo-random
		 * one (all the more as it might be a "dangerous"
		 * often firewalled port, such as 1214 as it happened
		 * to me once).
		 */
		IPPORT_TEREDO + 1;
#else
		0;
#endif
	bool ignore_cone = true;

	if (!ParseIPv4 (conf, "BindAddress", &bind_ip)
	 || !conf.GetInt16 ("BindPort", &bind_port)
	 || !conf.GetBoolean ("IgnoreConeBit", &ignore_cone))
	{
		syslog (LOG_ALERT, _("Fatal configuration error"));
		return -2;
	}

	bind_port = htons (bind_port);

	char *ifname = conf.GetRawValue ("InterfaceName");

	conf.Clear (5);

	/*
	 * SETUP
	 */

	// Tunneling interface initialization
	int fd = -1;
	tun6 *tunnel = (mode & TEREDO_CLIENT)
		? create_dynamic_tunnel (ifname, &fd)
		: create_static_tunnel (ifname, &prefix.ip6, mtu, mode == TEREDO_CONE);

	if (ifname != NULL)
		free (ifname);

	int retval = -1;

	if (tunnel == NULL)
	{
		syslog (LOG_ALERT, _("Miredo setup failure: %s"),
		        _("Cannot create IPv6 tunnel"));
		return -1;
	}

	if (miredo_init ((mode & TEREDO_CLIENT) != 0))
		syslog (LOG_ALERT, _("Miredo setup failure: %s"),
		        _("libteredo cannot be initialized"));
	else
	{
#ifdef MIREDO_TEREDO_CLIENT
		miredo_addrwatch *watch = (mode == TEREDO_EXCLIENT)
			? miredo_addrwatch_start (tun6_getId (tunnel)) : NULL;
#endif
		if (drop_privileges () == 0)
		{
			do
			{
				if (miredo_addrwatch_available (watch))
				{
					sigset_t s;
					sigemptyset (&s);

					if (pselect (0, NULL, NULL, NULL, watch_ts (watch), &s))
						retval = 0;
					else
						retval = -2;

					continue;
				}

				teredo_tunnel *relay = teredo_create (bind_ip, bind_port);
				if (relay != NULL)
				{
					miredo_tunnel data = { tunnel, fd };
					teredo_set_privdata (relay, &data);
					teredo_set_cone_ignore (relay, ignore_cone);
					teredo_set_recv_callback (relay, miredo_recv_callback);
					teredo_set_icmpv6_callback (relay, miredo_icmp6_callback);

					retval = (mode & TEREDO_CLIENT)
						? setup_client (relay, server_name, server_name2)
						: setup_relay (relay, prefix.teredo.prefix,
						               mode == TEREDO_CONE);
	
					/*
					 * RUN
					 */
					if (retval == 0)
					{
						retval = run_tunnel (relay, tunnel, watch);
#ifdef MIREDO_TEREDO_CLIENT
						if (retval == -2)
							miredo_down_callback (&data);
#endif
					}
					teredo_destroy (relay);
				}
			}
			while (retval == -2);

			if (retval)
				syslog (LOG_ALERT, _("Miredo setup failure: %s"),
				        _("libteredo cannot be initialized"));
		}

#ifdef MIREDO_TEREDO_CLIENT
		if (watch != NULL)
			miredo_addrwatch_stop (watch);
#endif
		miredo_deinit ((mode & TEREDO_CLIENT) != 0);
	}

#ifdef MIREDO_TEREDO_CLIENT
	if (fd != -1)
	{
		close (fd);
		wait (NULL); // wait for privsep process
	}
#endif
	tun6_destroy (tunnel);
	return retval;
}
