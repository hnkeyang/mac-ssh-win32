/*
	MacTelnet Connection Module - Shared connection logic
	Copyright (C) 2024

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "protocol.h"
#include "interfaces.h"
#include "utils.h"
#include "mndp.h"
#include "mactelnet_conn.h"

/* Connection state */
static SOCKET macrecv = INVALID_SOCKET;
static SOCKET macsend = INVALID_SOCKET;
static struct net_interface *outiface = NULL;
static unsigned int outcounter = 0;
static unsigned int incounter = 0;
static int keepalive_counter = 0;
static int running = 0;

/* Critical section protecting send_udp / outcounter from concurrent access */
static CRITICAL_SECTION conn_cs;
static BOOL conn_cs_initialized = FALSE;

/* Terminal callbacks */
static mactelnet_data_cb_t data_callback = NULL;
static mactelnet_status_cb_t status_callback = NULL;
static void *callback_userdata = NULL;

/* Forward declarations */
static int send_udp(struct mt_packet *packet, int retransmit, mactelnet_conn_t *conn);
static int handle_packet(struct mt_mactelnet_hdr *pkt, int data_len, mactelnet_conn_t *conn);
static int disconnect(mactelnet_conn_t *conn);

/* Parse MAC address string */
int parse_mac(const char *str, unsigned char *mac)
{
	int i;
	unsigned int vals[ETH_ALEN];
	
	/* Try colon-separated format */
	if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
		&vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) == ETH_ALEN) {
		for (i = 0; i < ETH_ALEN; i++)
			mac[i] = (unsigned char)vals[i];
		return 1;
	}
	
	/* Try dash-separated format */
	if (sscanf(str, "%02x-%02x-%02x-%02x-%02x-%02x",
		&vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) == ETH_ALEN) {
		for (i = 0; i < ETH_ALEN; i++)
			mac[i] = (unsigned char)vals[i];
		return 1;
	}
	
	return 0;
}

/* Initialize connection structure */
void mactelnet_conn_init(mactelnet_conn_t *conn)
{
	memset(conn, 0, sizeof(mactelnet_conn_t));
	conn->socket = INVALID_SOCKET;
	if (!conn_cs_initialized) {
		InitializeCriticalSection(&conn_cs);
		conn_cs_initialized = TRUE;
	}
}

/* Set callbacks */
void mactelnet_set_callbacks(mactelnet_data_cb_t data_cb, mactelnet_status_cb_t status_cb, void *userdata)
{
	data_callback = data_cb;
	status_callback = status_cb;
	callback_userdata = userdata;
}

/* Send UDP packet */
static int send_udp(struct mt_packet *packet, int retransmit, mactelnet_conn_t *conn)
{
	int sent_bytes;
	struct mt_mactelnet_hdr hdr = { };
	struct sockaddr_in socket_address;
	
	/* Clear keepalive counter */
	keepalive_counter = 0;
	
	/* Init SendTo struct */
	memset(&socket_address, 0, sizeof(socket_address));
	socket_address.sin_family = AF_INET;
	socket_address.sin_port = htons(MT_MACTELNET_PORT);
	socket_address.sin_addr = outiface->ipv4_bcast;
	
	sent_bytes = sendto(macsend, (char *)packet->data, packet->size, 0, 
		(struct sockaddr*)&socket_address, sizeof(socket_address));
	
	/* Retransmit logic */
	if (retransmit) {
		int i;
		for (i = 0; i < MAX_RETRANSMIT_INTERVALS; ++i) {
			if (net_readable(macrecv, retransmit_intervals[i])) {
				int result = net_recv_packet(macrecv, &hdr, NULL);
				if (result > 0 && handle_packet(&hdr, result, conn) == MT_PTYPE_ACK)
					return sent_bytes;
			}
			sendto(macsend, (char *)packet->data, packet->size, 0,
				(struct sockaddr*)&socket_address, sizeof(socket_address));
		}
		
		if (status_callback)
			status_callback(MT_STATUS_TIMEOUT, "Connection timed out", callback_userdata);
		return -1;
	}
	
	return sent_bytes;
}

/* Handle incoming data packet */
static int handle_data(struct mt_mactelnet_hdr *pkt, int data_len, mactelnet_conn_t *conn)
{
	struct mt_packet odata;
	struct mt_mactelnet_control_hdr cpkt;
	int success = 0;
	
	/* Always transmit ACKNOWLEDGE packets */
	init_packet(&odata, MT_PTYPE_ACK, outiface->mac_addr, conn->server_mac,
		conn->session_key, pkt->counter + (data_len - MT_HEADER_LEN));
	send_udp(&odata, 0, conn);
	
	/* Accept first packet and packets greater than incounter */
	if (incounter == 0 || pkt->counter > incounter || (incounter - pkt->counter) > 65535)
		incounter = pkt->counter;
	else
		return -1; /* Ignore double or old packets */
	
	/* Parse control packet data */
	success = parse_control_packet(pkt->data, data_len - MT_HEADER_LEN, &cpkt);
	
	while (success) {
		if (cpkt.cptype == MT_CPTYPE_PLAINDATA) {
			/* Send data to callback */
			if (data_callback)
				data_callback(cpkt.data, cpkt.length, callback_userdata);
		}
		else if (cpkt.cptype == MT_CPTYPE_END_AUTH) {
			if (status_callback)
				status_callback(MT_STATUS_CONNECTED, "Connected", callback_userdata);
		}
		
		/* Parse next control packet */
		success = parse_control_packet(NULL, 0, &cpkt);
	}
	
	return pkt->ptype;
}

/* Handle incoming packet */
static int handle_packet(struct mt_mactelnet_hdr *pkt, int data_len, mactelnet_conn_t *conn)
{
	/* We only care about packets with correct sessionkey */
	if (pkt->seskey != conn->session_key)
		return -1;
	
	switch (pkt->ptype) {
	case MT_PTYPE_DATA:
		return handle_data(pkt, data_len, conn);
		
	case MT_PTYPE_ACK:
		return MT_PTYPE_ACK;
		
	case MT_PTYPE_END:
		return disconnect(conn);
		
	default:
		return -1;
	}
}

/* Disconnect */
static int disconnect(mactelnet_conn_t *conn)
{
	struct mt_packet data;
	
	if (!outiface)
		return -1;
	
	/* Send END packet */
	init_packet(&data, MT_PTYPE_END, outiface->mac_addr, conn->server_mac,
		conn->session_key, 0);
	send_udp(&data, 0, conn);
	
	if (status_callback)
		status_callback(MT_STATUS_DISCONNECTED, "Disconnected", callback_userdata);
	
	conn->connected = 0;
	running = 0;
	return MT_PTYPE_END;
}

/* Find interface for connection */
static int find_interface(mactelnet_conn_t *conn, int timeout)
{
	struct mt_packet data;
	struct sockaddr_in local;
	int opt = 1;
	struct net_interface *iface;
	SOCKET mactest;
	
	list_for_each_entry(iface, &ifaces, list) {
		/* Initialize receiving socket on the device chosen */
		local.sin_family = AF_INET;
		local.sin_port = htons(conn->source_port);
		local.sin_addr = iface->ipv4_addr;
		
		/* Initialize socket and bind to udp port */
		mactest = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (mactest == INVALID_SOCKET)
			continue;
		
		setsockopt(mactest, SOL_SOCKET, SO_BROADCAST, (char *)&opt, sizeof(opt));
		setsockopt(mactest, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));
		
		if (bind(mactest, (struct sockaddr *)&local, sizeof(local)) == SOCKET_ERROR) {
			closesocket(mactest);
			continue;
		}
		
		/* Set the global socket handle and source mac address */
		macsend = mactest;
		outiface = iface;
		
		/* Send SESSIONSTART message */
		init_packet(&data, MT_PTYPE_SESSIONSTART, outiface->mac_addr, 
			conn->server_mac, conn->session_key, 0);
		send_udp(&data, 0, conn);
		
		/* Wait for response */
		if (net_readable(macrecv, timeout * 1000))
			return 1;
		
		closesocket(mactest);
	}
	return 0;
}

/* Setup MAC socket */
static int setup_mac_socket(int port)
{
	int opt = 1;
	int sock;
	struct sockaddr_in local;
	
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET)
		return INVALID_SOCKET;
	
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char *)&opt, sizeof(opt));
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));
	
	if (bind(sock, (struct sockaddr *)&local, sizeof(local)) == SOCKET_ERROR) {
		closesocket(sock);
		return INVALID_SOCKET;
	}
	
	return sock;
}

/* Connect to server */
int mactelnet_connect(mactelnet_conn_t *conn, const unsigned char *server_mac, int timeout)
{
	struct mt_packet data;
	struct mt_mactelnet_hdr hdr = { };
	int result;
	
	/* Copy server MAC */
	memcpy(conn->server_mac, server_mac, ETH_ALEN);
	
	/* Generate random source port and session key */
	srand((unsigned int)GetTickCount());
	conn->source_port = 1024 + (rand() % 1024);
	conn->session_key = rand() % 65535;
	
	/* Setup receive socket */
	macrecv = setup_mac_socket(conn->source_port);
	if (macrecv == INVALID_SOCKET) {
		if (status_callback)
			status_callback(MT_STATUS_ERROR, "Failed to create socket", callback_userdata);
		return -1;
	}
	
	/* Enumerate interfaces */
	net_enum_ifaces();
	
	/* Find interface and send SESSIONSTART */
	if (!find_interface(conn, timeout)) {
		closesocket(macrecv);
		if (status_callback)
			status_callback(MT_STATUS_ERROR, "No response from server", callback_userdata);
		return -1;
	}
	
	/* Wait for first packet */
	result = net_recv_packet(macrecv, &hdr, NULL);
	if (result < 1) {
		closesocket(macrecv);
		closesocket(macsend);
		if (status_callback)
			status_callback(MT_STATUS_ERROR, "Connection failed", callback_userdata);
		return -1;
	}
	
	/* Handle first packet */
	if (handle_packet(&hdr, result, conn) < 0) {
		closesocket(macrecv);
		closesocket(macsend);
		return -1;
	}
	
	/* Send BEGINAUTH */
	init_packet(&data, MT_PTYPE_DATA, outiface->mac_addr, conn->server_mac,
		conn->session_key, 0);
	outcounter += add_control_packet(&data, MT_CPTYPE_BEGINAUTH, NULL, 0);
	
	/* Send terminal info */
	const char *termtype = "xterm";
	uint16_t width = 80, height = 25;
	outcounter += add_control_packet(&data, MT_CPTYPE_TERM_TYPE, (void *)termtype, strlen(termtype));
	outcounter += add_control_packet(&data, MT_CPTYPE_TERM_WIDTH, &width, 2);
	outcounter += add_control_packet(&data, MT_CPTYPE_TERM_HEIGHT, &height, 2);
	
	send_udp(&data, 0, conn);
	
	conn->connected = 1;
	running = 1;
	
	if (status_callback)
		status_callback(MT_STATUS_CONNECTING, "Connecting...", callback_userdata);
	
	return 0;
}

/* Send data to server */
int mactelnet_send(mactelnet_conn_t *conn, const unsigned char *data, int len)
{
	struct mt_packet pkt;
	struct mt_mactelnet_hdr hdr = { };
	struct sockaddr_in socket_address;
	int i;
	
	if (!conn->connected)
		return -1;
	
	EnterCriticalSection(&conn_cs);
	
	/* Build the DATA packet */
	init_packet(&pkt, MT_PTYPE_DATA, outiface->mac_addr, conn->server_mac,
		conn->session_key, outcounter);
	add_control_packet(&pkt, MT_CPTYPE_PLAINDATA, (void *)data, len);
	outcounter += len;
	keepalive_counter = 0;
	
	memset(&socket_address, 0, sizeof(socket_address));
	socket_address.sin_family = AF_INET;
	socket_address.sin_port = htons(MT_MACTELNET_PORT);
	socket_address.sin_addr = outiface->ipv4_bcast;
	
	/* Initial send */
	sendto(macsend, (char *)pkt.data, pkt.size, 0,
		(struct sockaddr *)&socket_address, sizeof(socket_address));
	
	/* Retransmit loop using a LOCAL fd_set to avoid the global selectset race.
	 * conn_cs is held, so the background thread is blocked: we are the sole
	 * reader of macrecv here, guaranteeing we catch our own ACK. */
	for (i = 0; i < MAX_RETRANSMIT_INTERVALS; i++) {
		fd_set rset;
		struct timeval tv;
		int result;
		
		FD_ZERO(&rset);
		FD_SET(macrecv, &rset);
		tv.tv_sec  = retransmit_intervals[i] / 1000;
		tv.tv_usec = (retransmit_intervals[i] % 1000) * 1000;
		
		if (select(-1, &rset, NULL, NULL, &tv) > 0 && FD_ISSET(macrecv, &rset)) {
			result = net_recv_packet(macrecv, &hdr, NULL);
			if (result > 0) {
				if (handle_packet(&hdr, result, conn) == MT_PTYPE_ACK)
					break; /* server confirmed receipt */
				/* DATA packet received while waiting — ACK was sent by
				 * handle_data; keep waiting for our ACK */
			}
		} else {
			/* Timeout — retransmit */
			sendto(macsend, (char *)pkt.data, pkt.size, 0,
				(struct sockaddr *)&socket_address, sizeof(socket_address));
			keepalive_counter = 0;
		}
	}
	
	LeaveCriticalSection(&conn_cs);
	return (int)pkt.size;
}

/* Run connection loop - processes incoming data */
int mactelnet_run(mactelnet_conn_t *conn, int timeout_ms)
{
	struct mt_mactelnet_hdr hdr = { };
	int result;
	
	if (!conn->connected || !running)
		return -1;
	
	/* Wait for data without holding lock (non-blocking select) */
	if (net_select(timeout_ms, macrecv, INVALID_SOCKET) > 0) {
		if (net_readable(macrecv, -1)) {
			result = net_recv_packet(macrecv, &hdr, NULL);
			if (result > 0) {
				/* Lock while processing packet (which sends ACK via send_udp) */
				EnterCriticalSection(&conn_cs);
				handle_packet(&hdr, result, conn);
				LeaveCriticalSection(&conn_cs);
			}
		}
	} else {
		/* Keepalive */
		if (keepalive_counter++ == 10) {
			struct mt_packet odata;
			EnterCriticalSection(&conn_cs);
			init_packet(&odata, MT_PTYPE_ACK, outiface->mac_addr, conn->server_mac,
				conn->session_key, outcounter);
			send_udp(&odata, 0, conn);
			LeaveCriticalSection(&conn_cs);
		}
	}
	
	return running ? 0 : -1;
}

/* Disconnect */
void mactelnet_disconnect(mactelnet_conn_t *conn)
{
	if (conn->connected) {
		disconnect(conn);
		conn->connected = 0;
	}
	
	if (macrecv != INVALID_SOCKET) {
		closesocket(macrecv);
		macrecv = INVALID_SOCKET;
	}
	
	if (macsend != INVALID_SOCKET) {
		closesocket(macsend);
		macsend = INVALID_SOCKET;
	}
}
