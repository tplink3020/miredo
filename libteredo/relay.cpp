/*
 * relay.cpp - Teredo relay peers list definition
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

#include <string.h>
#include <time.h>

#if HAVE_STDINT_H
# include <stdint.h>
#elif HAVE_INTTYPES_H
# include <inttypes.h>
#endif

#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/ip6.h> // struct ip6_hdr
#include <syslog.h>

#include <libteredo/teredo.h>
#include <libteredo/v4global.h> // is_ipv4_global_unicast()
#include <libteredo/relay-udp.h>

#include "packets.h"
#include "security.h"
#include "queue.h"
#include <libteredo/relay.h>

#define TEREDO_TIMEOUT 30 // seconds

// is_valid_teredo_prefix (PREFIX_UNSET) MUST return false
#define PREFIX_UNSET 0xffffffff


#define MAXQUEUE 1280 // bytes


/*
 * Queueng of packets from the IPv6 toward Teredo
 */
class TeredoRelay::OutQueue : public PacketsQueue
{
	private:
		TeredoRelayUDP *sock;
		uint32_t addr;
		uint16_t port;

		virtual int SendPacket (const void *p, size_t len)
		{
			return sock->SendPacket (p, len, addr, port);
		}

	public:
		OutQueue (TeredoRelayUDP *s) : PacketsQueue (MAXQUEUE),
					       sock (s)
		{
		}

		void SetMapping (uint32_t mapped_addr, uint16_t mapped_port)
		{
			addr = mapped_addr;
			port = mapped_port;
		}
};

/*
 * Queueing of packets from Teredo toward the IPv6
 */
class TeredoRelay::InQueue : public PacketsQueue
{
	private:
		TeredoRelay *relay;

		virtual int SendPacket (const void *p, size_t len)
		{
			return relay->SendIPv6Packet (p, len);
		}

	public:
		InQueue (TeredoRelay *r) : PacketsQueue (MAXQUEUE), relay (r)
		{
		}
};



class TeredoRelay::peer
{
	private:
		struct timeval expiry;

	public:
		OutQueue outqueue;
		InQueue inqueue;

		peer (TeredoRelayUDP *sock, TeredoRelay *r)
			: outqueue (sock), inqueue (r)
		{
		}
		
		peer *next;

		struct in6_addr addr;
		uint32_t mapped_addr;
		uint16_t mapped_port;
		union
		{
			struct
			{
				unsigned trusted:1;
				unsigned replied:1;
				unsigned bubbles:2;
				unsigned pings:2;
				unsigned nonce:1; // mapped_* unset, nonce set
				unsigned dummy:9;
			} flags;
			uint16_t all_flags;
		} flags;
		// TODO: nonce and mapped_* could be union-ed
		uint8_t nonce[8]; /* only for client toward non-client */

	private:
		void Touch (void)
		{
			gettimeofday (&expiry, NULL);
			expiry.tv_sec += TEREDO_TIMEOUT;
		}

	public:
		void SetMapping (uint32_t ip, uint16_t port)
		{
			mapped_addr = ip;
			mapped_port = port;
			outqueue.SetMapping (ip, port);
		}

		void SetMappingFromPacket (const TeredoPacket& p)
		{
			SetMapping (p.GetClientIP (), p.GetClientPort ());
		}

		void TouchReceive (void)
		{
			flags.flags.replied = 1;
			Touch ();
		}

		void TouchTransmit (void)
		{
			if (flags.flags.replied == 0)
				Touch ();
		}

		bool IsExpired (const struct timeval& now) const
		{
			return ((signed)(now.tv_sec - expiry.tv_sec) > 0)
			     || ((now.tv_sec == expiry.tv_sec)
			      && (now.tv_usec >= expiry.tv_usec));
		}
};


#define PROBE_CONE	1
#define PROBE_RESTRICT	2
#define PROBE_SYMMETRIC	3

#define QUALIFIED	0


TeredoRelay::TeredoRelay (uint32_t pref, uint16_t port, uint32_t ipv4,
                          bool cone)
	:  head (NULL)
{
	addr.teredo.prefix = pref;
	addr.teredo.server_ip = 0;
	addr.teredo.flags = cone ? htons (TEREDO_FLAG_CONE) : 0;
	addr.teredo.client_ip = 0;
	addr.teredo.client_port = 0;

	sock.ListenPort (port, ipv4);
#ifdef MIREDO_TEREDO_CLIENT
	maintenance.state = QUALIFIED;
	maintenance.working = false;
#endif
}


#ifdef MIREDO_TEREDO_CLIENT
TeredoRelay::TeredoRelay (uint32_t ip, uint32_t ip2,
                          uint16_t port, uint32_t ipv4)
	: head (NULL), mtu (1280)
{
	if (!is_ipv4_global_unicast (ip) || !is_ipv4_global_unicast (ip2))
		syslog (LOG_WARNING, _("Server has a non global IPv4 address. "
		                       "It will most likely not work."));

	addr.teredo.prefix = PREFIX_UNSET;
	addr.teredo.server_ip = ip;
	addr.teredo.flags = htons (TEREDO_FLAG_CONE);
	addr.teredo.client_ip = 0;
	addr.teredo.client_port = 0;

	server_ip2 = ip2;

	maintenance.working = false;
	maintenance.state = PROBE_CONE;

	if (sock.ListenPort (port, ipv4) == 0)
	{
		pthread_mutex_init (&maintenance.lock, NULL);
		pthread_cond_init (&maintenance.received, NULL);
		if (pthread_create (&maintenance.thread, NULL, do_maintenance, this))
		{
			syslog (LOG_ALERT, _("pthread_create failure: %m"));
			pthread_cond_destroy (&maintenance.received);
			pthread_mutex_destroy (&maintenance.lock);
		}
		else
			maintenance.working = true;
	}
}

int TeredoRelay::NotifyUp (const struct in6_addr *, uint16_t mtu)
{
	return 0;
}


int TeredoRelay::NotifyDown (void)
{
	return 0;
}
#endif /* ifdef MIREDO_TEREDO_CLIENT */

/* Releases peers list entries */
TeredoRelay::~TeredoRelay (void)
{
#ifdef MIREDO_TEREDO_CLIENT
	if (maintenance.working)
	{
		maintenance.working = false;
		pthread_cancel (maintenance.thread);
		pthread_join (maintenance.thread, NULL);
		pthread_cond_destroy (&maintenance.received);
		pthread_mutex_destroy (&maintenance.lock);
	}
#endif

	peer *p = head;

	while (p != NULL)
	{
		peer *buf = p->next;
		delete p;
		p = buf;
	}
}


/* 
 * Allocates a peer entry. It is up to the caller to fill informations
 * correctly.
 *
 * FIXME: number of entry should be bound
 */
TeredoRelay::peer *TeredoRelay::AllocatePeer (const struct in6_addr *addr)
{
	struct timeval now;
	gettimeofday (&now, NULL);
	peer *p;

	/* Tries to recycle a timed-out peer entry */
	for (p = head; p != NULL; p = p->next)
		if (p->IsExpired (now))
		{
			p->outqueue.Trash ();
			p->inqueue.Trash ();
			break;
		}

	/* Otherwise allocates a new peer entry */
	if (p == NULL)
	{
		try
		{
			p = new peer (&sock, this);
		}
		catch (...)
		{
			return NULL;
		}

		/* Puts new entry at the head of the list */
		p->next = head;
		head = p;
	}

	memcpy (&p->addr, addr, sizeof (struct in6_addr));
	return p;
}


/*
 * Returns a pointer to the first peer entry matching <addr>,
 * or NULL if none were found.
 */
TeredoRelay::peer *TeredoRelay::FindPeer (const struct in6_addr *addr)
{
	struct timeval now;

	gettimeofday(&now, NULL);

	for (peer *p = head; p != NULL; p = p->next)
		if (memcmp (&p->addr, addr, sizeof (struct in6_addr)) == 0)
			if (!p->IsExpired (now))
				return p; // found!

	return NULL;
}


/*
 * Sends an ICMPv6 Destination Unreachable error to the IPv6 Internet.
 * Unfortunately, this will use a local-scope address as source, which is not
 * quite good.
 */
int
TeredoRelay::SendUnreach (int code, const void *in, size_t inlen)
{
	struct
	{
		struct ip6_hdr hdr;
		uint8_t fill[1280 - sizeof (struct ip6_hdr)];
	} buf;

	/* FIXME: implement ICMP rate limit */
	size_t outlen = BuildICMPv6Error (&buf.hdr, &teredo_cone, 1, code,
						in, inlen);
	return outlen ? SendIPv6Packet (&buf, outlen) : 0;
}


#ifdef MIREDO_TEREDO_CLIENT
int
TeredoRelay::PingPeer (peer *p) const
{
	if (!p->flags.flags.nonce)
	{
		if (!GenerateNonce (p->nonce))
			return -1;

		p->flags.flags.nonce = 1;
	}

	// FIXME: re-send echo request later if no response

	// FIXME FIXME FIXME:
	// - sending of pings should be done in a separate thread
	// - we don't check for the 2 seconds delay between pings
	if (p->flags.flags.pings < 3)
	{
		p->flags.flags.pings++;
		return SendPing (sock, &addr, &p->addr, p->nonce);
	}
	return 0;
}
#endif


/*
 * Returs true if the packet whose header is passed as a parameter looks
 * like a Teredo bubble.
 */
inline bool IsBubble (const struct ip6_hdr *hdr)
{
	return (hdr->ip6_plen == 0) && (hdr->ip6_nxt == IPPROTO_NONE);
}


/*
 * Handles a packet coming from the IPv6 Internet, toward a Teredo node
 * (as specified per paragraph 5.4.1). That's what the specification calls
 * "Packet transmission".
 * Returns 0 on success, -1 on error.
 */
int TeredoRelay::SendPacket (const void *packet, size_t length)
{
	struct ip6_hdr ip6;
	if ((length < sizeof (ip6)) || (length > 65507))
		return 0;

	memcpy (&ip6, packet, sizeof (ip6));

	// Sanity check (should we trust the kernel?):
	// It's no use emitting such a broken packet because the other side
	// will drop it anyway.
	if (((ip6.ip6_vfc >> 4) != 6)
	 || ((sizeof (ip6) + ntohs (ip6.ip6_plen)) != length))
		return 0; // invalid IPv6 packet

	/* Makes sure we are qualified properly */
	if (!IsRunning ())
		return SendUnreach (0, packet, length);

	const union teredo_addr *dst = (union teredo_addr *)&ip6.ip6_dst,
				*src = (union teredo_addr *)&ip6.ip6_src;

	if (dst->teredo.prefix != GetPrefix ()
	 && src->teredo.prefix != GetPrefix ())
		/*
		 * Routing packets not from a Teredo client,
		 * neither toward a Teredo client is NOT allowed through a
		 * Teredo tunnel. The Teredo server will reject the packet.
		 *
		 * We also drop link-local unicast and multicast packets as
		 * they can't be routed through Teredo properly.
		 */
		return SendUnreach (1, packet, length);


	peer *p = FindPeer (&ip6.ip6_dst);

	if (p != NULL)
	{
		/* Case 1 (paragraphs 5.2.4 & 5.4.1): trusted peer */
		if (p->flags.flags.trusted)
		{
			/* Already known -valid- peer */
			p->TouchTransmit ();
			return sock.SendPacket (packet, length,
						p->mapped_addr,
						p->mapped_port);
		}
	}
	
	/* Unknown or untrusted peer */
	if (dst->teredo.prefix != GetPrefix ())
	{
		/* Unkown or untrusted non-Teredo node */

		/*
		 * If we are not a qualified client, ie. we have no server
		 * IPv4 address to contact for direct IPv6 connectivity, we
		 * cannot route packets toward non-Teredo IPv6 addresses, and
		 * we are not allowed to do it by the specification either.
		 *
		 * NOTE:
		 * The specification mandates silently ignoring such
		 * packets. However, this only happens in case of
		 * misconfiguration, so I believe it could be better to
		 * notify the user. An alternative is to send an ICMPv6 error
		 * back to the kernel.
		 */
		if (IsRelay ())
			return SendUnreach (1, packet, length);

#ifdef MIREDO_TEREDO_CLIENT
		/* Client case 2: direct IPv6 connectivity test */
		// TODO: avoid code duplication
		if (p == NULL)
		{
			p = AllocatePeer (&ip6.ip6_dst);
			if (p == NULL)
				return -1; // memory error

			p->mapped_port = 0;
			p->mapped_addr = 0;
			p->flags.all_flags = 0;
			p->TouchTransmit ();
		}

		p->outqueue.Queue (packet, length);
		return PingPeer (p);
#endif
	}

	/* Unknown or untrusted Teredo client */

	// Ignores Teredo clients with incorrect server IPv4
	if (!is_ipv4_global_unicast (IN6_TEREDO_SERVER (&ip6.ip6_dst))
	 || (IN6_TEREDO_SERVER (&ip6.ip6_dst) == 0))
		return 0;

	/* Client case 3: TODO: implement local discovery */

	if (p == NULL)
	{
		/* Unknown Teredo clients */

		// Creates a new entry
		p = AllocatePeer (&ip6.ip6_dst);
		if (p == NULL)
			return -1; // insufficient memory

		p->SetMapping (IN6_TEREDO_IPV4 (dst), IN6_TEREDO_PORT (dst));
		p->flags.all_flags = 0;

		// NOTE: we call TouchTransmit() but if the peer is non-cone, and
		// we are cone, we don't actually send a packet
		p->TouchTransmit ();

		/* Client case 4 & relay case 2: new cone peer */
		if (IN6_IS_TEREDO_ADDR_CONE (&ip6.ip6_dst))
		{
			p->flags.flags.trusted = 1;
			return sock.SendPacket (packet, length,
						p->mapped_addr,
						p->mapped_port);
		}
	}

	/* Client case 5 & relay case 3: untrusted non-cone peer */
	p->outqueue.Queue (packet, length);

	// FIXME FIXME FIXME:
	// - sending of bubbles should be done in a separate thread
	// - we do no longer check for the 2 seconds delay between bubbles
	//   which really sucks

	// Sends no more than one bubble every 2 seconds,
	// and 3 bubbles every 30 secondes
	if (p->flags.flags.bubbles < 3)
	{
		//if (!p->flags.flags.bubbles || ((now - p->last_xmit) >= 2))
		p->flags.flags.bubbles ++;

		/*
		 * Open the return path if we are behind a
		 * restricted NAT.
		 */
		if (!IsCone () && SendBubble (sock, &ip6.ip6_dst, false, false))
			return -1;

		return SendBubble (sock, &ip6.ip6_dst, IsCone ());
	}

	// Too many bubbles already sent
	return 0;
}


/*
 * Handles a packet coming from the Teredo tunnel
 * (as specified per paragraph 5.4.2). That's called "Packet reception".
 * Returns 0 on success, -1 on error.
 */
#define SERVER_PING_DELAY 30

unsigned TeredoRelay::QualificationTimeOut = 4; // seconds
unsigned TeredoRelay::QualificationRetries = 3;
unsigned TeredoRelay::RestartDelay = 300; // seconds

int TeredoRelay::ReceivePacket (const fd_set *readset)
{
	TeredoPacket packet;

	if (sock.ReceivePacket (readset, packet))
		return -1;

	size_t length;
	const uint8_t *buf = packet.GetIPv6Packet (length);
	struct ip6_hdr ip6;

	// Checks packet
	if ((length < sizeof (ip6)) || (length > 65507))
		return 0; // invalid packet

	memcpy (&ip6, buf, sizeof (ip6));
	if (((ip6.ip6_vfc >> 4) != 6)
	 || ((ntohs (ip6.ip6_plen) + sizeof (ip6)) != length))
		return 0; // malformatted IPv6 packet

#ifdef MIREDO_TEREDO_CLIENT
	if (!IsRunning ())
		return ProcessQualificationPacket (&packet);

	/* Maintenance */
	if (IsClient () && IsServerPacket (&packet))
	{
		/*
		 * Server IP not checked because it might be the server's
		 * secondary IP. We use the authentication header instead.
		 */
		const uint8_t *s_nonce = packet.GetAuthNonce ();

		if (s_nonce != NULL)
		{
			pthread_mutex_lock (&maintenance.lock);
			if (memcmp (s_nonce, maintenance.nonce, 8))
			{
				pthread_mutex_unlock (&maintenance.lock);
				return 0; // server authentication failure
			}

			// Checks if our Teredo address changed:
			union teredo_addr newaddr;
			newaddr.teredo.server_ip = GetServerIP ();

			uint16_t new_mtu = mtu;

			if (ParseRA (packet, &newaddr, IsCone (), &new_mtu))
			{
				pthread_cond_signal (&maintenance.received);
				if (memcmp (&addr, &newaddr, sizeof (addr))
				 || (mtu != new_mtu))
				{
					memcpy (&addr, &newaddr, sizeof (addr));
					mtu = new_mtu;

					syslog (LOG_NOTICE, _("Teredo address/MTU changed"));
					NotifyUp (&newaddr.ip6, new_mtu);
				}
				pthread_mutex_unlock (&maintenance.lock);
				return 0;
			}
			pthread_mutex_unlock (&maintenance.lock);
		}

		const struct teredo_orig_ind *ind = packet.GetOrigInd ();
		if (ind != NULL)
		{
			SendBubble (sock, ~ind->orig_addr, ~ind->orig_port, &ip6.ip6_dst,
			            &ip6.ip6_src);
			if (IsBubble (&ip6))
				return 0; // don't pass bubble to kernel
		}
		else
		if (IsBubble (&ip6))
		{
			/*
			 * Some servers do not insert an origin indication.
			 * When the source IPv6 address is a Teredo address,
			 * we can guess the mapping. Otherwise, we're stuck.
			 */
		 	if (IN6_TEREDO_PREFIX (&ip6.ip6_src) == GetPrefix ())
				SendBubble (sock, IN6_TEREDO_IPV4 (&ip6.ip6_src),
				            IN6_TEREDO_PORT (&ip6.ip6_src), &ip6.ip6_dst,
				            &ip6.ip6_src);
			else
			{
				syslog (LOG_WARNING, _("Ignoring invalid bubble : "
				        "your Teredo server is probably buggy."));
			}
			return 0; // don't pass bubble to kernel
		}

		/*
		 * Normal reception of packet must only occur if it does not
		 * come from the server, as specified. However, it is not
		 * unlikely that our server is a relay too. Hence, we must
		 * further process packets from it.
		 * At the moment, we only drop bubble (see above).
		 * (NOTE: besides, we don't sufficiently check that the packet
		 *  comes from the server, we only check the source port)
		 */
	}
#endif /* MIREDO_TEREDO_CLIENT */

	/*
	 * NOTE/TODO:
	 * In the client case, the spec says we should check that the
	 * destination is our Teredo IPv6 address. However, this library makes
	 * no difference between relay, host-specific relay and client
	 * (it very much sounds like market segmentation to me).
	 * We purposedly leave it up to the kernel to determine whether he
	 * should accept, route, or drop the packet, according to its
	 * configuration. That should be done now if we wanted to.
	 *
	 * In the relay case, it says we should accept packet toward the range
	 * of hosts for which we serve as a Teredo relay, and should otherwise
	 * drop it. That should be done just before sending the packet. That
	 * might be a run-time option.
	 *
	 * It should be noted that dropping packets with link-local
	 * destination here, before further processing, breaks connectivity
	 * with restricted Teredo clients : we send them Teredo bubbles with
	 * a link-local source, to which they reply with Teredo bubbles with
	 * a link-local destination. Indeed, the specification specifies that
	 * the relay MUST look up the peer in the list and update last
	 * reception date even if the destination is incorrect.
	 */
#if 0
	/*
	 * Ensures that the packet destination has an IPv6 Internet scope
	 * (ie 2000::/3). That should be done just before calling
	 * SendIPv6Packet(), but it so much easier to do it now.
	 */
	if ((ip6.ip6_dst.s6_addr[0] & 0xe0) != 0x20)
		return 0; // must be discarded, or ICMPv6 error (?)

	if ((ip6.ip6_dst.s6_addr[0] & 0xfe) == 0xfe)
		return 0;
#endif
	/*
	 * Packets with a link-local source address are purposedly dropped to
	 * prevent the kernel from receiving faked Router Advertisement which
	 * could break IPv6 routing completely. Router advertisements MUST
	 * have a link-local source address (RFC 2461).
	 *
	 * Note: only Linux defines convenient s6_addr16, so we don't use it.
	 *
	 * In no case are relays and clients supposed to receive and process
	 * such a packet *except* from their server (that processing is done
	 * in the "Maintenance" case above), or bubbles from other restricted
	 * clients/relays, which can safely be ignored (so long as the other
	 * bubble sent through the server is not ignored).
	 *
	 * This check is not part of the Teredo specification.
	 */
	if ((((uint16_t *)ip6.ip6_src.s6_addr)[0] & 0xfec0) == 0xfe80)
		return 0;

	/* Actual packet reception, either as a relay or a client */

	// Checks source IPv6 address / looks up peer in the list:
	peer *p = FindPeer (&ip6.ip6_src);

	if (p != NULL)
	{
		// Client case 1 (trusted node or (trusted) Teredo client):
		if (p->flags.flags.trusted
		 && (packet.GetClientIP () == p->mapped_addr)
		 && (packet.GetClientPort () == p->mapped_port))
		{
			p->TouchReceive ();
			return SendIPv6Packet (buf, length);
		}

#ifdef MIREDO_TEREDO_CLIENT
		// Client case 2 (untrusted non-Teredo node):
		if ((!p->flags.flags.trusted) && p->flags.flags.nonce
		 && CheckPing (packet, p->nonce))
		{
			p->flags.flags.trusted = 1;
			p->flags.flags.nonce = 0;

			p->SetMappingFromPacket (packet);
			p->TouchReceive ();

			p->outqueue.Flush ();
			p->inqueue.Flush ();

			/*
			 * NOTE:
			 * This implies the kernel will see Echo replies sent
			 * for Teredo tunneling maintenance. It's not really
			 * an issue, as IPv6 stacks ignore them.
			 *
			 * FIXME: do not do this
			 */
			return SendIPv6Packet (buf, length);
		}
#endif /* ifdef MIREDO_TEREDO_CLIENT */
	}

	/*
	 * At this point, we have either a trusted mapping mismatch,
	 * an unlisted peer, or an un-trusted client peer.
	 */
	if (IN6_TEREDO_PREFIX (&ip6.ip6_src) == GetPrefix ())
	{
		// Client case 3 (unknown or untrusted matching Teredo client):
		if (IN6_MATCHES_TEREDO_CLIENT (&ip6.ip6_src,
						packet.GetClientIP (),
						packet.GetClientPort ()))
		{
			if (p == NULL)
			{
				/*
				 * Relays are explicitly allowed to drop
				 * packets from unknown peers and it is surely
				 * much better. It prevents routing of packet
				 * through the wrong relay.
				 */
				if (IsRelay ())
					return 0;

#ifdef MIREDO_TEREDO_CLIENT
				// TODO: do not duplicate this code
				p = AllocatePeer (&ip6.ip6_dst);
				if (p == NULL)
					return -1; // insufficient memory

				p->mapped_port =
					IN6_TEREDO_PORT (&ip6.ip6_dst);
				p->mapped_addr =
					IN6_TEREDO_IPV4 (&ip6.ip6_dst);
				p->outqueue.SetMapping (p->mapped_addr,
							p->mapped_port);
				p->flags.all_flags = 0;
#endif
			}
			else
			{
				p->outqueue.Flush ();
				/* p->inqueue.Flush (); -- always empty */
			}

			p->flags.flags.trusted = 1;
			p->TouchReceive ();

			if (IsBubble (&ip6))
				return 0; // discard Teredo bubble
			return SendIPv6Packet (buf, length);
		}

		// TODO: remove this line if we implement local teredo
		return 0;
	}

#ifdef MIREDO_TEREDO_CLIENT
	// Relays only accept packets from Teredo clients;
	if (IsRelay ())
		return 0;

	// TODO: implement client cases 4 & 5 for local Teredo

	/*
	 * Default: Client case 6:
	 * (unknown non-Teredo node or Tereco client with incorrect mapping):
	 * We should be cautious when accepting packets there, all the
	 * more as we don't know if we are a really client or just a
	 * qualified relay (ie. whether the host's default route is
	 * actually the Teredo tunnel).
	 */

	// TODO: avoid code duplication (direct IPv6 connectivity test)
	if (p == NULL)
	{
		p = AllocatePeer (&ip6.ip6_src);
		if (p == NULL)
			return -1; // memory error

		p->mapped_port = 0;
		p->mapped_addr = 0;
		p->flags.all_flags = 0;
	}

	p->inqueue.Queue (buf, length);
	p->TouchReceive ();

	return PingPeer (p);
#endif /* ifdef MIREDO_TEREDO_CLIENT */
	return 0;
}


#ifdef MIREDO_TEREDO_CLIENT
bool TeredoRelay::IsServerPacket (const TeredoPacket *packet) const
{
	uint32_t ip = packet->GetClientIP ();

	return (packet->GetClientPort () == htons (IPPORT_TEREDO))
	 && ((ip == GetServerIP ()) || (ip == GetServerIP2 ()));
}


/* Handle router advertisement for qualification */
int TeredoRelay::ProcessQualificationPacket (const TeredoPacket *packet)
{
	if (!IsServerPacket (packet))
		return 0;

	/*
	 * We don't accept router advertisement without nonce.
	 * It is far too easy to spoof such packets.
	 */
	const uint8_t *s_nonce = packet->GetAuthNonce ();

	if (s_nonce == NULL)
		return 0;

	union teredo_addr newaddr;
	newaddr.teredo.server_ip = GetServerIP ();
 
	pthread_mutex_lock (&maintenance.lock);
	if (memcmp (s_nonce, maintenance.nonce, 8))
	{
		pthread_mutex_unlock (&maintenance.lock);
		return 0;
	}

	if (packet->GetConfByte ())
	{
		pthread_mutex_unlock (&maintenance.lock);
		syslog (LOG_ERR, _("Authentication refused by server."));
		return 0;
	}

	if (!ParseRA (*packet, &newaddr, maintenance.state == PROBE_CONE, &mtu))
	{
		pthread_mutex_unlock (&maintenance.lock);
		return 0;
	}

	/* Valid router advertisement received! */
	pthread_cond_signal (&maintenance.received);

	if (maintenance.state == PROBE_SYMMETRIC)
	{
		maintenance.symmetric =
			 ((addr.teredo.client_port != newaddr.teredo.client_port)
			  || (addr.teredo.client_ip != newaddr.teredo.client_ip));
	}

	/* 
	 * newaddr (and mtu) must be copied before the lock is released,
	 * so that it is OK to call NotifyUp from the maintenance thread.
	 */
	memcpy (&addr, &newaddr, sizeof (addr));
	pthread_mutex_unlock (&maintenance.lock);

	return 0;
}


static void
asyncsafe_sleep (unsigned sec)
{
	struct timeval tv;
	int oldstate;

	tv.tv_sec = sec;
	tv.tv_usec = 0;
	pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, &oldstate);
	select (0, NULL, NULL, NULL, &tv);
	pthread_setcanceltype (oldstate, NULL);
	pthread_testcancel ();
}


void TeredoRelay::MaintenanceThread (void)
{
	bool cone = true;
	unsigned count = 0;

	// TODO: regenerate nonce from time to time
	pthread_mutex_lock (&maintenance.lock);
	GenerateNonce (maintenance.nonce, true);
	/* done in constructor -- maintenance.state = PROBE_CONE;*/

	/*
	 * Qualification/maintenance procedure
	 */
	while (1)
	{
		if ((maintenance.state == 0) && (count == 0))
		{
			pthread_mutex_unlock (&maintenance.lock);

			// TODO: randomize refresh interval
			asyncsafe_sleep (SERVER_PING_DELAY);

			pthread_mutex_lock (&maintenance.lock);
		}

		SendRS (sock, maintenance.state == PROBE_RESTRICT /* secondary */
		              ? GetServerIP2 () : GetServerIP (),
		        maintenance.nonce, cone);

		struct timespec deadline;
		{
			struct timeval tv;
			gettimeofday (&tv, NULL);
			deadline.tv_sec = tv.tv_sec + QualificationTimeOut;
			deadline.tv_nsec = tv.tv_usec * 1000;
		}

		if (pthread_cond_timedwait (&maintenance.received, &maintenance.lock,
		                            &deadline))
		{
			/* no response */
			if (maintenance.state == PROBE_SYMMETRIC)
				maintenance.state = PROBE_RESTRICT;
			else
				count++;

			if (count >= QualificationRetries)
			{
				bool down = (maintenance.state == 0);

				count = 0;
				if (maintenance.state == PROBE_CONE)
				{
					maintenance.state = PROBE_RESTRICT;
					cone = false;
				}
				else
				{
					maintenance.state = PROBE_CONE;
					cone = true;
				}

				if (down)
				{
					pthread_mutex_unlock (&maintenance.lock);
					syslog (LOG_NOTICE, _("Lost Teredo connectivity"));
					NotifyDown ();

					/* Sleep five minutes */
					asyncsafe_sleep (RestartDelay);
					pthread_mutex_lock (&maintenance.lock);
				}
			}
		}
		else
		if (maintenance.state)
		{
			if ((maintenance.state == PROBE_SYMMETRIC)
			 && maintenance.symmetric)
			{
				/* FAIL */
				count = 0;
				maintenance.state = PROBE_CONE;

				/* Sleep five minutes */
				pthread_mutex_unlock (&maintenance.lock);
				syslog (LOG_ERR, _("Unsupported symmetric NAT detected."));
				asyncsafe_sleep (RestartDelay);
				pthread_mutex_lock (&maintenance.lock);
			}
			else
			if (maintenance.state == PROBE_RESTRICT)
			{
				maintenance.state = PROBE_SYMMETRIC;
			}
			else
			{
				syslog (LOG_INFO, _("Qualified (NAT type: %s)"),
				        gettext (cone ? N_("cone") : N_("restricted")));
				count = 0;
				maintenance.state = 0;
				// FIXME: do this in the main thread
				// FIXME: ensure that is completed before the main thread continue works
				// FIXME: prevent possible maintenance.lock dead locking
				NotifyUp (&addr.ip6, mtu);
			}
		}
	}
}


void *
TeredoRelay::do_maintenance (void *data)
{
	((TeredoRelay *)data)->MaintenanceThread ();
	return NULL;
}

#endif /* ifdef MIREDO_TEREDO_CLIENT */
