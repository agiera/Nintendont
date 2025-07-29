/*
Slippi FTP Client for Nintendont
Simplified FTP client for uploading replay files
Based on ftpii FTP implementation
*/

#ifndef _SLIPPI_FTP_H_
#define _SLIPPI_FTP_H_

#include "Slippi.h"
#include "ff_utf8.h"

// FTP client return codes
#define SLIPPI_FTP_SUCCESS		0
#define SLIPPI_FTP_ERROR		-1
#define SLIPPI_FTP_CONNECT_FAIL	-2
#define SLIPPI_FTP_AUTH_FAIL	-3
#define SLIPPI_FTP_UPLOAD_FAIL	-4

// Maximum queue size for pending uploads
#define SLIPPI_FTP_QUEUE_SIZE	32

// Structure for queued replay files
typedef struct {
	char filepath[256];
	char filename[64];
	int queued;
} slippi_ftp_queue_entry_t;

// FTP client state
typedef struct {
	int socket;
	int connected;
	int authenticated;
	char response_buffer[512];
} slippi_ftp_client_t;

// Function prototypes
int slippi_ftp_init(void);
void slippi_ftp_cleanup(void);
int slippi_ftp_queue_replay(const char* filepath);
int slippi_ftp_upload_queued_replays(void);
void slippi_ftp_cancel_uploads(void);
int slippi_ftp_get_queue_count(void);

// Internal functions
static int slippi_ftp_connect(slippi_ftp_client_t* client, const char* server, unsigned short port);
static int slippi_ftp_authenticate(slippi_ftp_client_t* client, const char* username, const char* password);
static int slippi_ftp_upload_file(slippi_ftp_client_t* client, const char* local_path, const char* remote_path);
static int slippi_ftp_read_response(slippi_ftp_client_t* client);
static int slippi_ftp_send_command(slippi_ftp_client_t* client, const char* command);
static void slippi_ftp_disconnect(slippi_ftp_client_t* client);

#endif /* _SLIPPI_FTP_H_ */
