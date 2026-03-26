/*
	SSH Connection module for MacConnect
	Uses libssh for SSH terminal connections
*/

#ifndef SSH_CONN_H
#define SSH_CONN_H

#include <winsock2.h>
#include <windows.h>
#include <libssh/libssh.h>

/* SSH connection structure */
typedef struct ssh_conn {
	ssh_session session;
	ssh_channel channel;
	int connected;
	int sockfd;
	char host[256];
	int port;
	char last_error[512]; /* last libssh error string for diagnostics */
} ssh_conn_t;

/* Callback types */
typedef void (*ssh_data_callback_t)(const unsigned char *data, int len, void *userdata);
typedef void (*ssh_status_callback_t)(int status, const char *msg, void *userdata);

/* Status codes - use high base to avoid collision with MT_STATUS_* enum values */
#define SSH_STATUS_CONNECTED        100
#define SSH_STATUS_ERROR            101
#define SSH_STATUS_DISCONNECTED     102
#define SSH_STATUS_AUTH_FAILED      103
#define SSH_STATUS_HOST_KEY_UNKNOWN 104

/* Functions */
void ssh_conn_init(ssh_conn_t *conn);
int ssh_conn_connect(ssh_conn_t *conn, const char *host, int port);
int ssh_conn_auth_interactive(ssh_conn_t *conn);
int ssh_conn_auth_password(ssh_conn_t *conn, const char *user, const char *password);
int ssh_conn_open_channel(ssh_conn_t *conn);
int ssh_conn_send(ssh_conn_t *conn, const unsigned char *data, int len);
int ssh_conn_run(ssh_conn_t *conn, int timeout_ms);
void ssh_conn_disconnect(ssh_conn_t *conn);
void ssh_conn_set_callbacks(ssh_data_callback_t data_cb, ssh_status_callback_t status_cb, void *userdata);

#endif /* SSH_CONN_H */
