/*
 * teredo-udp.cpp - UDP sockets class definition
 * $Id$
 *
 * See "Teredo: Tunneling IPv6 over UDP through NATs"
 * for more information
 */

/***********************************************************************
 *  Copyright (C) 2004-2005 Remi Denis-Courmont.                       *
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

#include <gettext.h>

#if HAVE_STDINT_H
# include <stdint.h>
#elif HAVE_INTTYPES_H
# include <inttypes.h>
#endif
#include <string.h> // memcpy()

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h> // close()
#include <fcntl.h>
#include <sys/select.h>
#include <netinet/in.h>

#include <syslog.h> // syslog()

#include "teredo-udp.h"

#ifndef SOL_IP
# define SOL_IP IPPROTO_IP
#endif

/*
 * Opens a Teredo UDP/IPv4 socket.
 */
int
TeredoPacket::OpenSocket (uint32_t bind_ip, uint16_t port)
{
	struct sockaddr_in myaddr = { };
	myaddr.sin_family = AF_INET;
	myaddr.sin_port = port;
	myaddr.sin_addr.s_addr = bind_ip;
#ifdef HAVE_SA_LEN
	myaddr.sin_len = sizeof (myaddr);
#endif

	int fd = socket (PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd == -1)
	{
		syslog (LOG_ALERT, _("Fatal socket error: %m"));
		return -1; // failure
	}

	int flags = fcntl (fd, F_GETFL, 0);
	if (flags != -1)
		fcntl (fd, F_SETFL, O_NONBLOCK | flags);

	if (bind (fd, (struct sockaddr *)&myaddr, sizeof (myaddr)))
	{
		syslog (LOG_ALERT, _("Fatal bind error: %m"));
		return -1;
	}

	flags = 1;
	setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof (flags));
#ifdef IP_PMTUDISC_DONT
	/* 
	 * This tells the (Linux) kernel not to set the Don't Fragment flags
	 * on UDP packets we send. This is recommended by the Teredo
	 * specifiation.
	 */
	flags = IP_PMTUDISC_DONT;
	setsockopt (fd, SOL_IP, IP_MTU_DISCOVER, &flags, sizeof (flags));
#endif
	/*
	 * Teredo multicast packets always have a TTL of 1.
	 */
	setsockopt (fd, SOL_IP, IP_MULTICAST_TTL, &flags, sizeof (flags));
	return fd;
}


void
TeredoPacket::CloseSocket (int fd)
{
	if (fd != -1)
		close (fd);
}


int
TeredoPacket::Send (int fd, const void *packet, size_t plen,
			uint32_t dest_ip, uint16_t dest_port)
{
	struct sockaddr_in nat_addr;

	nat_addr.sin_family = AF_INET;
	nat_addr.sin_port = dest_port;
	nat_addr.sin_addr.s_addr = dest_ip;
#ifdef HAVE_SA_LEN
	nat_addr.sin_len = sizeof (nat_addr);
#endif

	return sendto (fd, packet, plen, 0, (struct sockaddr *)&nat_addr,
			sizeof (nat_addr)) == (int)plen ? 0 : -1;
}


/*** TeredoPacket implementation ***/

/*
 * Parses a Teredo packet header. Blocking function.
 */
int
TeredoPacket::Receive (int fd)
{
	int length;

	// Receive a UDP packet
	{
		struct sockaddr_in ad;
		socklen_t alen = sizeof (ad);

		length = recvfrom (fd, buf, sizeof (buf), 0,
					(struct sockaddr *)&ad, &alen);

		if (length < 0)
			return -1;

		last_ip = ad.sin_addr.s_addr;
		last_port = ad.sin_port;
	}

	// Check type of Teredo header:
	uint8_t *ptr = buf;
	orig = NULL;
	nonce = NULL;

	// Parse Teredo headers
	if (length < 40)
		return -1; // too small

	// Teredo Authentication header
	if ((ptr[0] == 0) && (ptr[1] == teredo_auth_hdr))
	{
		ptr += 2;
		length -= 13;
		if (length < 0)
			return -1; // too small

		/* ID and Auth */
		uint8_t id_len = *ptr++;
		uint8_t au_len = *ptr++;

		length -= id_len + au_len;
		if (length < 0)
			return -1;

		/* TODO: secure qualification */
		ptr += id_len + au_len;

		/* Nonce + confirmation byte */
		nonce = ptr;
		ptr += 9;
	}

	// Teredo Origin Indication
	if ((ptr[0] == 0) && (ptr[1] == teredo_orig_ind))
	{
		length -= sizeof (orig_buf);
		if (length < 0)
			return -1; // too small

		memcpy (&orig_buf, ptr, sizeof (orig_buf));
		orig = &orig_buf;
		ptr += sizeof (orig_buf);
	}

	// length <= 65507 = sizeof(buf)
	ip6len = length;
	ip6 = ptr;

	return 0;
}


int
TeredoPacket::ReceiveBlocking (int fd)
{
	fd_set readset;
	int val;

	FD_ZERO (&readset);
	FD_SET (fd, &readset);
	val = select (fd + 1, &readset, NULL, NULL, NULL);
	return (val == 1) ? Receive (fd) : -1;
}
