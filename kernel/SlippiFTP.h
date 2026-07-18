/*
Slippi FTP Client for Nintendont
Simplified FTP client for uploading replay files
Based on ftpii FTP implementation
*/

#ifndef _SLIPPI_FTP_H_
#define _SLIPPI_FTP_H_

#include "../common/include/Slippi.h"
#include "ff_utf8.h"
#include "global.h"

// FTP client return codes
#define SLIPPI_FTP_SUCCESS		0
#define SLIPPI_FTP_ERROR		-1
#define SLIPPI_FTP_CONNECT_FAIL	-2
#define SLIPPI_FTP_AUTH_FAIL	-3
#define SLIPPI_FTP_UPLOAD_FAIL	-4

// FTP client state
typedef struct {
    int socket;
    int connected;
    int authenticated;
    char response_buffer[512];
} slippi_ftp_client_t;

// Streaming upload state
typedef struct {
    int active;
    int data_socket;
    char remote_filename[256];
    char local_filepath[256];
    u32 bytes_uploaded;
    slippi_ftp_client_t* client;
} slippi_ftp_stream_t;

// Public function prototypes
int slippi_ftp_init(void);
void slippi_ftp_cleanup(void);

// Streaming upload functions
int slippi_ftp_start_stream_upload(const char* local_path, const char* remote_path);
int slippi_ftp_stream_data(const void* data, u32 size);
int slippi_ftp_finish_stream_upload(void);
void slippi_ftp_cancel_stream_upload(void);
int slippi_ftp_is_stream_active(void);

// Send the per-game controller-metadata sidecar over its own short-lived FTP
// connection. Used to defer the sidecar until the controller metadata is
// actually populated in shared memory (the active stream's control connection
// is busy with the live replay transfer, so a separate connection is used).
int slippi_ftp_send_metadata_sidecar_now(const char* remote_replay_path);

// Upload a finalized replay file in one shot (preserves patched raw_len header).
int slippi_ftp_upload_replay_file(const char* local_path, const char* remote_path);

#endif /* _SLIPPI_FTP_H_ */