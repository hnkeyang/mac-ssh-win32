/*
	SSH Connection module for MacConnect
	Uses libssh for SSH terminal connections
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <libssh/libssh.h>
#include <libssh/callbacks.h>

#include "ssh_conn.h"

/* Global callbacks */
static ssh_data_callback_t g_data_callback = NULL;
static ssh_status_callback_t g_status_callback = NULL;
static void *g_userdata = NULL;

/* Forward declarations */
static int verify_knownhost(ssh_session session);

void ssh_conn_init(ssh_conn_t *conn)
{
	memset(conn, 0, sizeof(ssh_conn_t));
	conn->session = NULL;
	conn->channel = NULL;
	conn->connected = 0;
	conn->sockfd = -1;
}

void ssh_conn_set_callbacks(ssh_data_callback_t data_cb, ssh_status_callback_t status_cb, void *userdata)
{
	g_data_callback = data_cb;
	g_status_callback = status_cb;
	g_userdata = userdata;
}

int ssh_conn_connect(ssh_conn_t *conn, const char *host, int port)
{
	int rc;
	long timeout = 10;

	/* Initialize libssh */
	ssh_session session = ssh_new();
	if (session == NULL) {
		strncpy(conn->last_error, "ssh_new() returned NULL", sizeof(conn->last_error) - 1);
		return -1;
	}

	/* Set options */
	ssh_options_set(session, SSH_OPTIONS_HOST, host);
	{
		unsigned int uport = (unsigned int)port;
		ssh_options_set(session, SSH_OPTIONS_PORT, &uport);
	}
	ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);

	/* Wrap ssh_connect in an SEH frame: libssh on Windows can raise
	 * STATUS_INVALID_HANDLE (0xc0000008) when WSACreateEvent()/WSACloseEvent()
	 * is called on a NULL or already-closed event handle in an error path. */
	rc = SSH_OK;
	__try {
		rc = ssh_connect(session);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		DWORD ecode = GetExceptionCode();
		snprintf(conn->last_error, sizeof(conn->last_error),
			"ssh_connect raised SEH exception 0x%08lx"
			" (libssh WinSock handle error)", (unsigned long)ecode);
		/* Try to release the session; ignore any further exception. */
		__try { ssh_free(session); } __except (EXCEPTION_EXECUTE_HANDLER) { /* ignore */ }
		return -1;
	}
	if (rc != SSH_OK) {
		snprintf(conn->last_error, sizeof(conn->last_error),
			"ssh_connect: %s", ssh_get_error(session));
		ssh_free(session);
		return -1;
	}

	/* Verify host key */
	rc = verify_knownhost(session);
	if (rc < 0) {
		snprintf(conn->last_error, sizeof(conn->last_error),
			"verify_knownhost: %s", ssh_get_error(session));
		ssh_disconnect(session);
		ssh_free(session);
		return -1;
	}

	conn->session = session;
	strncpy(conn->host, host, sizeof(conn->host) - 1);
	conn->port = port;
	conn->last_error[0] = '\0';

	return 0;
}

/* Verify known host - automatically accept for now */
static int verify_knownhost(ssh_session session)
{
	/* For now, auto-accept all host keys */
	/* In a real application, you'd want to verify the fingerprint */
	return 0;
}

int ssh_conn_auth_interactive(ssh_conn_t *conn)
{
	int rc;
	
	/* Get username - we'll prompt via terminal */
	/* For now, we'll use keyboard-interactive authentication */
	/* The actual prompts will be handled by the terminal */
	
	rc = ssh_userauth_none(conn->session, NULL);
	if (rc == SSH_AUTH_SUCCESS) {
		return 0;
	}
	
	/* Try keyboard-interactive */
	int method = ssh_userauth_list(conn->session, NULL);
	
	if (method & SSH_AUTH_METHOD_PASSWORD) {
		/* Will be handled by terminal - return special code */
		return 1; /* Need password */
	}
	
	if (method & SSH_AUTH_METHOD_INTERACTIVE) {
		return 2; /* Need keyboard-interactive */
	}
	
	if (g_status_callback)
		g_status_callback(SSH_STATUS_AUTH_FAILED, "No supported authentication method", g_userdata);
	
	return -1;
}

int ssh_conn_auth_password(ssh_conn_t *conn, const char *user, const char *password)
{
	int rc;
	
	rc = ssh_userauth_password(conn->session, user, password);
	if (rc == SSH_AUTH_SUCCESS) {
		return 0;
	}
	
	if (g_status_callback) {
		char msg[256];
		snprintf(msg, sizeof(msg), "Authentication failed: %s", ssh_get_error(conn->session));
		g_status_callback(SSH_STATUS_AUTH_FAILED, msg, g_userdata);
	}
	
	return -1;
}

int ssh_conn_open_channel(ssh_conn_t *conn)
{
	int rc;

	conn->channel = ssh_channel_new(conn->session);
	if (conn->channel == NULL) {
		snprintf(conn->last_error, sizeof(conn->last_error),
			"ssh_channel_new: %s", ssh_get_error(conn->session));
		return -1;
	}

	rc = ssh_channel_open_session(conn->channel);
	if (rc != SSH_OK) {
		snprintf(conn->last_error, sizeof(conn->last_error),
			"open_session: %s", ssh_get_error(conn->session));
		ssh_channel_free(conn->channel);
		conn->channel = NULL;
		return -1;
	}

	/* Request PTY */
	rc = ssh_channel_request_pty_size(conn->channel, "xterm", 80, 25);
	if (rc != SSH_OK) {
		snprintf(conn->last_error, sizeof(conn->last_error),
			"request_pty: %s", ssh_get_error(conn->session));
		ssh_channel_close(conn->channel);
		ssh_channel_free(conn->channel);
		conn->channel = NULL;
		return -1;
	}

	/* Start shell */
	rc = ssh_channel_request_shell(conn->channel);
	if (rc != SSH_OK) {
		snprintf(conn->last_error, sizeof(conn->last_error),
			"request_shell: %s", ssh_get_error(conn->session));
		ssh_channel_close(conn->channel);
		ssh_channel_free(conn->channel);
		conn->channel = NULL;
		return -1;
	}

	conn->connected = 1;
	conn->last_error[0] = '\0';
	return 0;
}

int ssh_conn_send(ssh_conn_t *conn, const unsigned char *data, int len)
{
	if (!conn->connected || conn->channel == NULL)
		return -1;
	
	int rc = ssh_channel_write(conn->channel, data, len);
	if (rc == SSH_ERROR) {
		conn->connected = 0;
		return -1;
	}
	
	return rc;
}

int ssh_conn_run(ssh_conn_t *conn, int timeout_ms)
{
	if (!conn->connected || conn->channel == NULL)
		return -1;
	
	char buffer[4096];
	int rc;
	
	/* Check if channel is closed or EOF */
	if (ssh_channel_is_closed(conn->channel) || ssh_channel_is_eof(conn->channel)) {
		conn->connected = 0;
		return -1;
	}
	
	/* Read data (non-blocking) */
	rc = ssh_channel_read_nonblocking(conn->channel, buffer, sizeof(buffer), 0);
	
	if (rc == SSH_ERROR) {
		conn->connected = 0;
		return -1;
	}
	
	if (rc == 0) {
		/* No data yet, not an error */
		return 0;
	}
	
	if (rc > 0 && g_data_callback) {
		g_data_callback((unsigned char *)buffer, rc, g_userdata);
	}
	
	return 0;
}

void ssh_conn_disconnect(ssh_conn_t *conn)
{
	if (conn->channel) {
		ssh_channel_close(conn->channel);
		ssh_channel_free(conn->channel);
		conn->channel = NULL;
	}
	
	if (conn->session) {
		ssh_disconnect(conn->session);
		ssh_free(conn->session);
		conn->session = NULL;
	}
	
	conn->connected = 0;
}
