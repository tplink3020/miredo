/*
 * relay.h - Linux Teredo relay implementation
 * $Id: relay.h,v 1.9 2004/08/17 19:09:51 rdenisc Exp $
 *
 * See "Teredo: Tunneling IPv6 over UDP through NATs"
 * for more information
 */

/***********************************************************************
 *  Copyright (C) 2004 Remi Denis-Courmont.                            *
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

#ifndef MIREDO_RELAY_H
# define MIREDO_RELAY_H

# ifndef __cplusplus
#  error C++ only header
# endif

# include <inttypes.h>
# include <libteredo/relay.h>

class IPv6Tunnel;

class MiredoRelay : public TeredoRelay
{
	private:
		const IPv6Tunnel *tunnel;
		int priv_fd;

		virtual int SendIPv6Packet (const void *packet,
						size_t length);
		virtual int NotifyUp (const struct in6_addr *addr);
		virtual int NotifyDown (void);

	public:
		MiredoRelay (const IPv6Tunnel *tun, uint32_t prefix,
				uint16_t port);

		//virtual void ~MiredoRelay (void);
};

#endif