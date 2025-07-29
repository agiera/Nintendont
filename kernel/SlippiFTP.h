/*
Slippi FTP Client for Nintendont
Simplified FTP client for uploading replay files
Based on ftpii FTP implementation
*/

#ifndef _SLIPPI_FTP_H_
#define _SLIPPI_FTP_H_

#include "../common/include/Slippi.h"
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

// Public function prototypes
int slippi_ftp_init(void);
void slippi_ftp_cleanup(void);
int slippi_ftp_queue_replay(const char* filepath);
int slippi_ftp_upload_queued_replays(void);
void slippi_ftp_cancel_uploads(void);
int slippi_ftp_get_queue_count(void);

#endif /* _SLIPPI_FTP_H_ */