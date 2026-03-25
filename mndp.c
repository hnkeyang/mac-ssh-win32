/*
	MacSSH - MAC-Telnet SSH Connect utility for Windows
	Copyright (C) 2014, Jo-Philipp Wich <jow@openwrt.org>

	Based on MAC-Telnet with SSH extension support.
	Copyright (C) 2011, Ali Onur Uyar <aouyar@gmail.com>

	Based on MAC-Telnet implementation for Linux.
	Copyright (C) 2010, Håkon Nessjøen <haakon.nessjoen@gmail.com>


	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License along
	with this program; if not, write to the Free Software Foundation, Inc.,
	51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "utils.h"
#include "protocol.h"
#include "interfaces.h"
#include "mndp.h"

LIST_HEAD(mndphosts);

int mndp_discover(int timeout)
{
	int rv, sock, opt = 1, found = 0;
	struct net_interface *iface;
	struct mndphost *mndphost;
	struct mt_mndp_info *mndppkt;
	struct sockaddr_in local, remote;
	unsigned char buf[MT_PACKET_LEN];
	char *address, *identity, *platform, *version, *hardware;

	local.sin_family = AF_INET;
	local.sin_port = htons(MT_MNDP_PORT);
	local.sin_addr.s_addr = htonl(INADDR_ANY);

	remote.sin_family = AF_INET;
	remote.sin_port = htons(MT_MNDP_PORT);

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (sock == SOCKET_ERROR)
		goto err;

	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char *)&opt, sizeof(opt));

	if (bind(sock, (struct sockaddr *)&local, sizeof(local)) == SOCKET_ERROR)
		goto err;

	list_for_each_entry(iface, &ifaces, list)
	{
		remote.sin_addr = iface->ipv4_bcast;

		sendto(sock, (char *)"\0\0\0\0", 4, 0,
		       (struct sockaddr *)&remote, sizeof(remote));
	}

	while (1)
	{
		rv = net_readable(sock, timeout * 1000);

		if (rv == SOCKET_ERROR)
			goto err;

		if (rv == 0)
			break;

		rv = recvfrom(sock, (char *)buf, sizeof(buf), 0, NULL, NULL);

		if (rv == SOCKET_ERROR)
			continue;

		mndppkt = parse_mndp(buf, rv);

		if (!mndppkt)
			continue;

		/* already seen */
		if (mndp_lookup(mndppkt->address))
			continue;

		mndphost = calloc_a(sizeof(*mndphost), &address, ETH_ALEN,
			&identity, mndppkt->identity ? 1 + strlen(mndppkt->identity) : 0,
			&platform, mndppkt->platform ? 1 + strlen(mndppkt->platform) : 0,
			&version,  mndppkt->version  ? 1 + strlen(mndppkt->version)  : 0,
			&hardware, mndppkt->hardware ? 1 + strlen(mndppkt->hardware) : 0,
			NULL);

		if (!mndphost)
			continue;

		mndphost->uptime  = mndppkt->uptime;
		mndphost->address = memcpy(address,  mndppkt->address, ETH_ALEN);

		if (mndppkt->identity)
			mndphost->identity = strcpy(identity, mndppkt->identity);

		if (mndppkt->platform)
			mndphost->platform = strcpy(platform, mndppkt->platform);

		if (mndppkt->version)
			mndphost->version  = strcpy(version,  mndppkt->version);

		if (mndppkt->hardware)
			mndphost->hardware = strcpy(hardware, mndppkt->hardware);

		/* Store IP addresses if present */
		mndphost->has_ipv4 = mndppkt->has_ipv4;
		mndphost->has_ipv6_local = mndppkt->has_ipv6_local;
		mndphost->has_ipv6_global = mndppkt->has_ipv6_global;

		if (mndppkt->has_ipv4) {
			mndphost->ipv4_addr = malloc(sizeof(struct in_addr));
			if (mndphost->ipv4_addr)
				memcpy(mndphost->ipv4_addr, &mndppkt->ipv4_addr, sizeof(struct in_addr));
		}

		if (mndppkt->has_ipv6_local) {
			mndphost->ipv6_local = malloc(sizeof(struct in6_addr));
			if (mndphost->ipv6_local)
				memcpy(mndphost->ipv6_local, &mndppkt->ipv6_local, sizeof(struct in6_addr));
		}

		if (mndppkt->has_ipv6_global) {
			mndphost->ipv6_global = malloc(sizeof(struct in6_addr));
			if (mndphost->ipv6_global)
				memcpy(mndphost->ipv6_global, &mndppkt->ipv6_global, sizeof(struct in6_addr));
		}

		list_add_tail(&mndphost->list, &mndphosts);
		found++;
	}

	closesocket(sock);

	return found;

err:
	if (sock != SOCKET_ERROR)
		closesocket(sock);

	return -1;
}

struct mndphost * mndp_lookup(const unsigned char *address)
{
	struct mndphost *host;

	list_for_each_entry(host, &mndphosts, list)
		if (!memcmp(host->address, address, ETH_ALEN))
			return host;

	return NULL;
}

void mndp_free_hosts(void)
{
	struct mndphost *host, *tmp;

	list_for_each_entry_safe(host, tmp, &mndphosts, list)
	{
		if (host->has_ipv4 && host->ipv4_addr)
			free(host->ipv4_addr);
		if (host->has_ipv6_local && host->ipv6_local)
			free(host->ipv6_local);
		if (host->has_ipv6_global && host->ipv6_global)
			free(host->ipv6_global);

		list_del(&host->list);
		free(host);
	}
}

void mndp_list(int timeout, int batch_mode)
{
	struct mndphost *host;
	int n = 0;

	/* Discover hosts */
	mndp_discover(timeout);

	if (batch_mode) {
		printf("MAC-Address,Identity,Platform,Version,Hardware,Uptime,Softid,Ifname,IPv4,IPv6-Local,IPv6-Global\n");
		list_for_each_entry(host, &mndphosts, list)
		{
			printf("'%02X:%02X:%02X:%02X:%02X:%02X','%s',",
				host->address[0], host->address[1], host->address[2],
				host->address[3], host->address[4], host->address[5],
				host->identity ? host->identity : "");
			printf("'%s','%s','%s',",
				host->platform ? host->platform : "",
				host->version ? host->version : "",
				host->hardware ? host->hardware : "");
			printf("'%d','','',",
				host->uptime);
			/* Print IP addresses */
			if (host->has_ipv4 && host->ipv4_addr) {
				printf("'%s',", inet_ntoa(*host->ipv4_addr));
			} else {
				printf("','");
			}
			if (host->has_ipv6_local && host->ipv6_local) {
				char ipv6_str[INET6_ADDRSTRLEN];
				inet_ntop(AF_INET6, host->ipv6_local, ipv6_str, sizeof(ipv6_str));
				printf("'%s',", ipv6_str);
			} else {
				printf("','");
			}
			if (host->has_ipv6_global && host->ipv6_global) {
				char ipv6_str[INET6_ADDRSTRLEN];
				inet_ntop(AF_INET6, host->ipv6_global, ipv6_str, sizeof(ipv6_str));
				printf("'%s'", ipv6_str);
			} else {
				printf("''");
			}
			printf("\n");
		}
	} else {
		printf("\n%-17s %s\n", "MAC-Address", "Identity (platform version hardware) uptime");
		list_for_each_entry(host, &mndphosts, list)
		{
			n++;
			printf(" %2d) %02X:%02X:%02X:%02X:%02X:%02X %s",
				n,
				host->address[0], host->address[1], host->address[2],
				host->address[3], host->address[4], host->address[5],
				host->identity ? host->identity : "");

			if (host->platform && host->version && host->hardware)
				printf(" (%s %s %s)", host->platform, host->version, host->hardware);

			if (host->uptime)
				printf("  up %d days %02d:%02d:%02d",
					host->uptime / 86400, host->uptime % 86400 / 3600,
					host->uptime % 3600 / 60, host->uptime % 60);

			/* Print IP addresses */
			if (host->has_ipv4 && host->ipv4_addr)
				printf(" [%s]", inet_ntoa(*host->ipv4_addr));
			if (host->has_ipv6_local && host->ipv6_local) {
				char ipv6_str[INET6_ADDRSTRLEN];
				inet_ntop(AF_INET6, host->ipv6_local, ipv6_str, sizeof(ipv6_str));
				printf(" [%s]", ipv6_str);
			}
			if (host->has_ipv6_global && host->ipv6_global) {
				char ipv6_str[INET6_ADDRSTRLEN];
				inet_ntop(AF_INET6, host->ipv6_global, ipv6_str, sizeof(ipv6_str));
				printf(" [%s]", ipv6_str);
			}

			printf("\n");
		}
		if (n == 0)
			printf("No hosts found\n");
	}
}
