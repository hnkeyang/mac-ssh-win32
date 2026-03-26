/*
	MacTelnet Connection Module - Header
	Copyright (C) 2024

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
*/

#ifndef _MACTELNET_CONN_H
#define _MACTELNET_CONN_H

#include <winsock2.h>
#include "protocol.h"

/* Status codes */
enum mactelnet_status {
	MT_STATUS_CONNECTING,
	MT_STATUS_CONNECTED,
	MT_STATUS_DISCONNECTED,
	MT_STATUS_ERROR,
	MT_STATUS_TIMEOUT
};

/* Connection structure */
typedef struct {
	unsigned char server_mac[ETH_ALEN];
	int source_port;
	int session_key;
	int connected;
	SOCKET socket;
} mactelnet_conn_t;

/* Callback types */
typedef void (*mactelnet_data_cb_t)(const unsigned char *data, int len, void *userdata);
typedef void (*mactelnet_status_cb_t)(int status, const char *msg, void *userdata);

/* Function prototypes */
int parse_mac(const char *str, unsigned char *mac);
void mactelnet_conn_init(mactelnet_conn_t *conn);
void mactelnet_set_callbacks(mactelnet_data_cb_t data_cb, mactelnet_status_cb_t status_cb, void *userdata);
int mactelnet_connect(mactelnet_conn_t *conn, const unsigned char *server_mac, int timeout);
int mactelnet_send(mactelnet_conn_t *conn, const unsigned char *data, int len);
int mactelnet_run(mactelnet_conn_t *conn, int timeout_ms);
void mactelnet_disconnect(mactelnet_conn_t *conn);

#endif /* _MACTELNET_CONN_H */
