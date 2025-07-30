/*
Slippi FTP Client for Nintendont
Full FTP client for uploading replay files
Based on ftpii FTP implementation
*/

#include "SlippiFTP.h"
#include "Config.h"
#include "string.h"
#include "syscalls.h"
#include "vsprintf.h"
#include "net.h"
#include "debug.h"
#include "alloc.h"
#include "ff_utf8.h"

#define FTP_BUFFER_SIZE 1024
#define CRLF "\r\n"
#define CRLF_LENGTH 2

// Simple implementations for missing string functions
static char* strrchr_impl(const char* str, int c) {
	char* last = NULL;
	while (*str) {
		if (*str == c) {
			last = (char*)str;
		}
		str++;
	}
	return last;
}

static char* strcat_impl(char* dest, const char* src) {
	char* d = dest;
	while (*d) d++; // Find end of dest
	while ((*d++ = *src++)); // Copy src to end of dest
	return dest;
}

// Global state
static slippi_ftp_queue_entry_t ftp_queue[SLIPPI_FTP_QUEUE_SIZE];
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;
static slippi_ftp_client_t ftp_client;
static int ftp_initialized = 0;
static int upload_in_progress = 0;

// Streaming upload state
static slippi_ftp_stream_t stream_upload;
static slippi_ftp_client_t stream_client;

// Function prototypes for internal functions
static int slippi_ftp_connect(slippi_ftp_client_t* client, const char* server, unsigned short port);
static int slippi_ftp_authenticate(slippi_ftp_client_t* client, const char* username, const char* password);
static int slippi_ftp_upload_file(slippi_ftp_client_t* client, const char* local_path, const char* remote_path);
static int slippi_ftp_read_response(slippi_ftp_client_t* client);
static int slippi_ftp_send_command(slippi_ftp_client_t* client, const char* command);
static void slippi_ftp_disconnect(slippi_ftp_client_t* client);
static int transfer_exact(int socket, char *buf, int length, int is_send);
static int send_from_file(int data_socket, const char* filepath);
static void slippi_ftp_clear_responses(slippi_ftp_client_t* client);

// Initialize FTP client system
int slippi_ftp_init(void) {
	dbgprintf("FTP: Starting initialization...\r\n");
	
	if (ftp_initialized) {
		dbgprintf("FTP: Already initialized\r\n");
		return SLIPPI_FTP_SUCCESS;
	}
	
	dbgprintf("FTP: Clearing queue and client state...\r\n");
	// Clear queue
	memset(ftp_queue, 0, sizeof(ftp_queue));
	queue_head = 0;
	queue_tail = 0;
	queue_count = 0;
	
	// Clear client state
	memset(&ftp_client, 0, sizeof(ftp_client));
	ftp_client.socket = -1;
	ftp_client.connected = 0;
	ftp_client.authenticated = 0;
	
	// Clear streaming state
	memset(&stream_upload, 0, sizeof(stream_upload));
	stream_upload.data_socket = -1;
	memset(&stream_client, 0, sizeof(stream_client));
	stream_client.socket = -1;
	
	upload_in_progress = 0;
	ftp_initialized = 1;
	
	dbgprintf("FTP: Initialization complete\r\n");
	return SLIPPI_FTP_SUCCESS;
}

// Cleanup FTP client system
void slippi_ftp_cleanup(void) {
	if (!ftp_initialized) {
		return;
	}
	
	// Cancel any active streaming upload
	slippi_ftp_cancel_stream_upload();
	
	slippi_ftp_disconnect(&ftp_client);
	
	// Clear queue
	memset(ftp_queue, 0, sizeof(ftp_queue));
	queue_head = 0;
	queue_tail = 0;
	queue_count = 0;
	
	ftp_initialized = 0;
	upload_in_progress = 0;
}

// Add a replay file to the upload queue
int slippi_ftp_queue_replay(const char* filepath) {
	if (!filepath) {
		return SLIPPI_FTP_ERROR;
	}
	
	if (!ftp_initialized) {
		// If FTP is not initialized (disabled), just return success to avoid errors
		return SLIPPI_FTP_SUCCESS;
	}
	
	if (queue_count >= SLIPPI_FTP_QUEUE_SIZE) {
		return SLIPPI_FTP_ERROR;
	}
	
	// Extract filename from full path
	const char* filename = strrchr_impl(filepath, '/');
	if (!filename) {
		filename = filepath;
	} else {
		filename++; // Skip the '/'
	}
	
	// Add to queue
	strncpy(ftp_queue[queue_tail].filepath, filepath, sizeof(ftp_queue[queue_tail].filepath) - 1);
	strncpy(ftp_queue[queue_tail].filename, filename, sizeof(ftp_queue[queue_tail].filename) - 1);
	ftp_queue[queue_tail].queued = 1;
	
	queue_tail = (queue_tail + 1) % SLIPPI_FTP_QUEUE_SIZE;
	queue_count++;
	
	return SLIPPI_FTP_SUCCESS;
}

// Upload all queued replay files
int slippi_ftp_upload_queued_replays(void) {
	if (!ftp_initialized || queue_count == 0 || upload_in_progress) {
		dbgprintf("FTP: Upload skipped - initialized=%d, queue_count=%d, in_progress=%d\r\n", 
			ftp_initialized, queue_count, upload_in_progress);
		return SLIPPI_FTP_ERROR;
	}
	
	// Check if FTP is enabled in config
	if (!slippi_settings || !slippi_settings->ftp_enabled) {
		dbgprintf("FTP: Upload skipped - settings unavailable or FTP disabled\r\n");
		return SLIPPI_FTP_ERROR;
	}
	
	// Debug FTP settings
	dbgprintf("FTP: Attempting upload with server=%s, port=%d, user=%s\r\n",
		slippi_settings->ftp_server, slippi_settings->ftp_port, slippi_settings->ftp_username);
	dbgprintf("FTP: FTP password=%s, directory=%s\r\n",
		slippi_settings->ftp_password, slippi_settings->ftp_directory);
	dbgprintf("FTP: FTP enabled=%d\r\n", slippi_settings->ftp_enabled);
	
	upload_in_progress = 1;
	
	// Connect to FTP server
	dbgprintf("FTP: Connecting to %s:%d\r\n", slippi_settings->ftp_server, slippi_settings->ftp_port);
	if (slippi_ftp_connect(&ftp_client, slippi_settings->ftp_server, slippi_settings->ftp_port) != SLIPPI_FTP_SUCCESS) {
		upload_in_progress = 0;
		dbgprintf("FTP: Connection failed\r\n");
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	
	// Authenticate
	if (slippi_ftp_authenticate(&ftp_client, slippi_settings->ftp_username, slippi_settings->ftp_password) != SLIPPI_FTP_SUCCESS) {
		slippi_ftp_disconnect(&ftp_client);
		upload_in_progress = 0;
		return SLIPPI_FTP_AUTH_FAIL;
	}
	
	// Upload each file in queue
	int uploaded = 0;
	int failed = 0;
	while (queue_count > 0) {
		slippi_ftp_queue_entry_t* entry = &ftp_queue[queue_head];
		
		if (entry->queued) {
			// Construct remote path
			char remote_path[256];
			if (strlen(slippi_settings->ftp_directory) > 0 && strcmp(slippi_settings->ftp_directory, "/") != 0) {
				// Directory is specified and not root, so add directory + filename
				_sprintf(remote_path, "%s/%s", slippi_settings->ftp_directory, entry->filename);
			} else {
				// Directory is root or empty, just use filename
				strncpy(remote_path, entry->filename, sizeof(remote_path) - 1);
				remote_path[sizeof(remote_path) - 1] = '\0';
			}
			
			dbgprintf("FTP: Uploading %s to %s\r\n", entry->filepath, remote_path);
			
			// Check if file exists before attempting upload
			FIL test_file;
			FRESULT file_check = f_open_secondary_drive(&test_file, entry->filepath, FA_READ);
			if (file_check != FR_OK) {
				dbgprintf("FTP: File access failed: %s (FRESULT=%d)\r\n", entry->filepath, file_check);
				
				// Try multiple times with increasing delays - file might still be open
				int retry_attempts = 3;
				int retry;
				for (retry = 0; retry < retry_attempts; retry++) {
					dbgprintf("FTP: Retry attempt %d/%d\r\n", retry + 1, retry_attempts);
					
					// Longer delay for each retry
					volatile int delay_count;
					for (delay_count = 0; delay_count < (500000 * (retry + 1)); delay_count++);
					
					// Retry file access
					file_check = f_open_secondary_drive(&test_file, entry->filepath, FA_READ);
					if (file_check == FR_OK) {
						f_close(&test_file);
						dbgprintf("FTP: File access successful after retry %d\r\n", retry + 1);
						
						// Upload file
						if (slippi_ftp_upload_file(&ftp_client, entry->filepath, remote_path) == SLIPPI_FTP_SUCCESS) {
							uploaded++;
						} else {
							failed++;
						}
						break;
					}
				}
				
				if (file_check != FR_OK) {
					dbgprintf("FTP: File not accessible after %d retries, skipping: %s (FRESULT=%d)\r\n", retry_attempts, entry->filepath, file_check);
					failed++;
				}
			} else {
				f_close(&test_file);
				
				// Upload file
				if (slippi_ftp_upload_file(&ftp_client, entry->filepath, remote_path) == SLIPPI_FTP_SUCCESS) {
					uploaded++;
				} else {
					failed++;
				}
			}
		}
		
		// Remove from queue
		entry->queued = 0;
		queue_head = (queue_head + 1) % SLIPPI_FTP_QUEUE_SIZE;
		queue_count--;
	}
	
	slippi_ftp_disconnect(&ftp_client);
	upload_in_progress = 0;
	
	dbgprintf("FTP: Upload complete - uploaded: %d, failed: %d\r\n", uploaded, failed);
	return uploaded > 0 ? SLIPPI_FTP_SUCCESS : SLIPPI_FTP_UPLOAD_FAIL;
}

// Cancel any uploads in progress
void slippi_ftp_cancel_uploads(void) {
	if (!ftp_initialized) {
		return;
	}
	
	if (upload_in_progress) {
		slippi_ftp_disconnect(&ftp_client);
		upload_in_progress = 0;
	}
}

// Get number of files in upload queue
int slippi_ftp_get_queue_count(void) {
	return queue_count;
}

// Connect to FTP server
static int slippi_ftp_connect(slippi_ftp_client_t* client, const char* server, unsigned short port) {
	extern s32 top_fd;
	
	if (!client || !server) {
		dbgprintf("FTP: Invalid client or server parameter\r\n");
		return SLIPPI_FTP_ERROR;
	}
	
	// Check network status before attempting socket creation
	dbgprintf("FTP: Checking network status, top_fd = %d\r\n", top_fd);
	if (top_fd < 0) {
		dbgprintf("FTP: Network not initialized (top_fd = %d)\r\n", top_fd);
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	
	// Create socket
	dbgprintf("FTP: Creating socket with top_fd = %d\r\n", top_fd);
	client->socket = socket(top_fd, AF_INET, SOCK_STREAM, IPPROTO_IP);
	dbgprintf("FTP: Socket creation returned %d\r\n", client->socket);
	
	if (client->socket < 0) {
		dbgprintf("FTP: Failed to create socket (error %d)\r\n", client->socket);
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	
	// Set up server address
	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_len = 8;
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = port;
	
	// Parse IP address manually
	u32 ip_addr = 0;
	int h1 = 0, h2 = 0, h3 = 0, h4 = 0;
	const char* p = server;
	
	// Parse first octet
	while (*p >= '0' && *p <= '9') h1 = h1 * 10 + (*p++ - '0');
	if (*p++ != '.') goto ip_error;
	
	// Parse second octet
	while (*p >= '0' && *p <= '9') h2 = h2 * 10 + (*p++ - '0');
	if (*p++ != '.') goto ip_error;
	
	// Parse third octet
	while (*p >= '0' && *p <= '9') h3 = h3 * 10 + (*p++ - '0');
	if (*p++ != '.') goto ip_error;
	
	// Parse fourth octet
	while (*p >= '0' && *p <= '9') h4 = h4 * 10 + (*p++ - '0');
	if (*p != '\0') goto ip_error;
	
	// Validate ranges
	if (h1 > 255 || h2 > 255 || h3 > 255 || h4 > 255) goto ip_error;
	
	ip_addr = (h1 << 24) | (h2 << 16) | (h3 << 8) | h4;
	dbgprintf("FTP: Parsed IP: %d.%d.%d.%d, port: %d\r\n", h1, h2, h3, h4, port);
	memcpy(&server_addr.sin_addr, &ip_addr, sizeof(ip_addr));
	goto ip_success;
	
ip_error:
	dbgprintf("FTP: Invalid IP address format\r\n");
	close(top_fd, client->socket);
	client->socket = -1;
	return SLIPPI_FTP_CONNECT_FAIL;
	
ip_success:
	
	// Connect to server
	dbgprintf("FTP: Attempting TCP connect to %d.%d.%d.%d:%d\r\n", h1, h2, h3, h4, port);
	s32 connect_result = connect(top_fd, client->socket, (struct sockaddr*)&server_addr);
	dbgprintf("FTP: Connect result: %d\r\n", connect_result);
	if (connect_result < 0) {
		dbgprintf("FTP: Failed to connect to server (error %d)\r\n", connect_result);
		close(top_fd, client->socket);
		client->socket = -1;
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	dbgprintf("FTP: TCP connection established\r\n");
	
	client->connected = 1;
	
	// Read welcome message (220)
	dbgprintf("FTP: Reading welcome message...\r\n");
	int response_code = slippi_ftp_read_response(client);
	dbgprintf("FTP: Welcome message response code: %d\r\n", response_code);
	if (response_code != 220) {
		dbgprintf("FTP: Invalid welcome message: %d\r\n", response_code);
		slippi_ftp_disconnect(client);
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	
	dbgprintf("FTP: Connection successful!\r\n");
	return SLIPPI_FTP_SUCCESS;
}

// Send file data over socket - based on ftpii's send_from_file
static int send_from_file(int data_socket, const char* filepath) {
	FIL file;
	FRESULT result = f_open_secondary_drive(&file, filepath, FA_READ);
	if (result != FR_OK) {
		dbgprintf("FTP: Failed to open file for upload: %s\r\n", filepath);
		return -1;
	}
	
	// Get file size for progress reporting
	FSIZE_t file_size = f_size(&file);
	dbgprintf("FTP: Starting upload of %s (%d bytes)\r\n", filepath, (int)file_size);
	
	char buffer[1024]; // Similar to ftpii's FREAD_BUFFER_SIZE
	UINT bytes_read;
	int total_sent = 0;
	int send_result = 0;
	int chunks_sent = 0;
	int progress_report_interval = 10; // Report every 10 chunks (10KB)
	
	while (f_read(&file, buffer, sizeof(buffer), &bytes_read) == FR_OK && bytes_read > 0) {
		send_result = transfer_exact(data_socket, buffer, bytes_read, 1);
		if (send_result < 0) {
			dbgprintf("FTP: Failed to send file data at byte %d\r\n", total_sent);
			break;
		}
		total_sent += bytes_read;
		chunks_sent++;
		
		// Report progress every so often
		if (chunks_sent % progress_report_interval == 0) {
			int percent = file_size > 0 ? (total_sent * 100) / file_size : 0;
			dbgprintf("FTP: Upload progress: %d/%d bytes (%d%%)\r\n", total_sent, (int)file_size, percent);
		}
		
		// Check if we read less than buffer size (end of file)
		if (bytes_read < sizeof(buffer)) {
			send_result = 0; // Success
			break;
		}
	}
	
	f_close(&file);
	
	if (send_result >= 0) {
		dbgprintf("FTP: Successfully sent %d bytes (100%%)\r\n", total_sent);
		return 0;
	} else {
		return send_result;
	}
}
static int slippi_ftp_authenticate(slippi_ftp_client_t* client, const char* username, const char* password) {
	dbgprintf("FTP: Starting authentication...\r\n");
	
	if (!client->connected) {
		dbgprintf("FTP: Authentication failed - not connected\r\n");
		return SLIPPI_FTP_ERROR;
	}
	
	// Send USER command
	char user_cmd[128];
	_sprintf(user_cmd, "USER %s", username);
	
	dbgprintf("FTP: Sending USER command...\r\n");
	if (slippi_ftp_send_command(client, user_cmd) != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send USER command\r\n");
		return SLIPPI_FTP_AUTH_FAIL;
	}
	
	// Read response to USER command (expect 331 "User name okay, need password" or 230 "User logged in")
	dbgprintf("FTP: Reading USER response...\r\n");
	int response_code = slippi_ftp_read_response(client);
	dbgprintf("FTP: USER response code: %d\r\n", response_code);
	
	if (response_code == 230) {
		// Anonymous user is already logged in, no password needed
		dbgprintf("FTP: Anonymous user already logged in\r\n");
		client->authenticated = 1;
		return SLIPPI_FTP_SUCCESS;
	} else if (response_code != 331) {
		dbgprintf("FTP: USER command failed with code %d\r\n", response_code);
		return SLIPPI_FTP_AUTH_FAIL;
	}
	
	// Send PASS command
	char pass_cmd[128];
	_sprintf(pass_cmd, "PASS %s", password);
	
	dbgprintf("FTP: Sending PASS command...\r\n");
	if (slippi_ftp_send_command(client, pass_cmd) != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send PASS command\r\n");
		return SLIPPI_FTP_AUTH_FAIL;
	}
	
	// Read response to PASS command (expect 230 "User logged in, proceed")
	dbgprintf("FTP: Reading PASS response...\r\n");
	response_code = slippi_ftp_read_response(client);
	dbgprintf("FTP: PASS response code: %d\r\n", response_code);
	if (response_code != 230) {
		dbgprintf("FTP: PASS command failed with code %d\r\n", response_code);
		return SLIPPI_FTP_AUTH_FAIL;
	}
	
	client->authenticated = 1;
	dbgprintf("FTP: Authentication successful\r\n");
	return SLIPPI_FTP_SUCCESS;
}

// Upload a file via FTP - based on ftpii's STOR implementation
static int slippi_ftp_upload_file(slippi_ftp_client_t* client, const char* local_path, const char* remote_path) {
	extern s32 top_fd;
	
	if (!client->connected || !client->authenticated) {
		dbgprintf("FTP: Upload failed - not connected or authenticated\r\n");
		return SLIPPI_FTP_ERROR;
	}
	
	dbgprintf("FTP: Starting upload process for %s\r\n", local_path);
	
	// Set binary mode (TYPE I)
	dbgprintf("FTP: Sending TYPE I command\r\n");
	if (slippi_ftp_send_command(client, "TYPE I") != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send TYPE command\r\n");
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	dbgprintf("FTP: Reading TYPE response\r\n");
	int response_code = slippi_ftp_read_response(client);
	dbgprintf("FTP: TYPE response code: %d\r\n", response_code);
	if (response_code != 200) {
		dbgprintf("FTP: TYPE command failed with code %d\r\n", response_code);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Enter passive mode (PASV)
	dbgprintf("FTP: Sending PASV command\r\n");
	if (slippi_ftp_send_command(client, "PASV") != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send PASV command\r\n");
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	dbgprintf("FTP: Reading PASV response\r\n");
	response_code = slippi_ftp_read_response(client);
	dbgprintf("FTP: PASV response code: %d\r\n", response_code);
	if (response_code != 227) {
		dbgprintf("FTP: PASV command failed with code %d\r\n", response_code);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	dbgprintf("FTP: PASV response: %s\r\n", client->response_buffer);
	
	// Parse PASV response "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)"
	dbgprintf("FTP: Parsing PASV response for data connection info\r\n");
	char* pasv_start = strstr(client->response_buffer, "(");
	if (!pasv_start) {
		dbgprintf("FTP: Invalid PASV response format\r\n");
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Parse PASV values manually
	int h1 = 0, h2 = 0, h3 = 0, h4 = 0, p1 = 0, p2 = 0;
	const char* p = pasv_start + 1;
	
	// Parse h1
	while (*p >= '0' && *p <= '9') h1 = h1 * 10 + (*p++ - '0');
	if (*p++ != ',') goto pasv_error;
	
	// Parse h2
	while (*p >= '0' && *p <= '9') h2 = h2 * 10 + (*p++ - '0');
	if (*p++ != ',') goto pasv_error;
	
	// Parse h3
	while (*p >= '0' && *p <= '9') h3 = h3 * 10 + (*p++ - '0');
	if (*p++ != ',') goto pasv_error;
	
	// Parse h4
	while (*p >= '0' && *p <= '9') h4 = h4 * 10 + (*p++ - '0');
	if (*p++ != ',') goto pasv_error;
	
	// Parse p1
	while (*p >= '0' && *p <= '9') p1 = p1 * 10 + (*p++ - '0');
	if (*p++ != ',') goto pasv_error;
	
	// Parse p2
	while (*p >= '0' && *p <= '9') p2 = p2 * 10 + (*p++ - '0');
	if (*p != ')') goto pasv_error;
	
	// Validate ranges
	if (h1 > 255 || h2 > 255 || h3 > 255 || h4 > 255 || p1 > 255 || p2 > 255) goto pasv_error;
	
	int data_port = (p1 << 8) | p2;
	dbgprintf("FTP: Parsed PASV data connection: %d.%d.%d.%d:%d\r\n", h1, h2, h3, h4, data_port);
	goto pasv_success;
	
pasv_error:
	dbgprintf("FTP: Failed to parse PASV response\r\n");
	return SLIPPI_FTP_UPLOAD_FAIL;
	
pasv_success:
	
	// Create data connection socket
	dbgprintf("FTP: Creating data connection socket\r\n");
	s32 data_socket = socket(top_fd, AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (data_socket < 0) {
		dbgprintf("FTP: Failed to create data socket (error %d)\r\n", data_socket);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	dbgprintf("FTP: Data socket created: %d\r\n", data_socket);
	
	// Set up data connection address
	struct sockaddr_in data_addr;
	memset(&data_addr, 0, sizeof(data_addr));
	data_addr.sin_len = 8;
	data_addr.sin_family = AF_INET;
	data_addr.sin_port = (p1 << 8) | p2;
	
	u32 data_ip = (h1 << 24) | (h2 << 16) | (h3 << 8) | h4;
	memcpy(&data_addr.sin_addr, &data_ip, sizeof(data_ip));
	
	// Connect to data port
	dbgprintf("FTP: Connecting to data port %d.%d.%d.%d:%d\r\n", h1, h2, h3, h4, data_port);
	s32 data_connect_result = connect(top_fd, data_socket, (struct sockaddr*)&data_addr);
	if (data_connect_result < 0) {
		dbgprintf("FTP: Failed to connect to data port (error %d)\r\n", data_connect_result);
		close(top_fd, data_socket);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	dbgprintf("FTP: Data connection established\r\n");
	
	// Send STOR command
	char stor_cmd[256];
	_sprintf(stor_cmd, "STOR %s", remote_path);
	
	dbgprintf("FTP: Sending STOR command: %s\r\n", stor_cmd);
	if (slippi_ftp_send_command(client, stor_cmd) != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send STOR command\r\n");
		close(top_fd, data_socket);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Read initial response (should be 150 "Opening data connection")
	dbgprintf("FTP: Reading STOR response\r\n");
	response_code = slippi_ftp_read_response(client);
	dbgprintf("FTP: STOR response code: %d\r\n", response_code);
	if (response_code != 150) {
		dbgprintf("FTP: STOR command failed with code %d\r\n", response_code);
		close(top_fd, data_socket);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Transfer file data
	dbgprintf("FTP: Starting file data transfer\r\n");
	int file_result = send_from_file(data_socket, local_path);
	dbgprintf("FTP: File transfer result: %d\r\n", file_result);
	
	// Close data connection
	dbgprintf("FTP: Closing data connection\r\n");
	close(top_fd, data_socket);
	
	if (file_result < 0) {
		dbgprintf("FTP: File transfer failed\r\n");
		// Read any pending response to clear the connection
		slippi_ftp_read_response(client);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Read final response (should be 226 "Transfer complete")
	dbgprintf("FTP: Reading final transfer response\r\n");
	response_code = slippi_ftp_read_response(client);
	dbgprintf("FTP: Final transfer response code: %d\r\n", response_code);
	if (response_code != 226) {
		dbgprintf("FTP: Transfer completion failed with code %d\r\n", response_code);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	dbgprintf("FTP: Successfully uploaded %s\r\n", remote_path);
	return SLIPPI_FTP_SUCCESS;
}

// Read response from FTP server - properly handle multi-line responses
static int slippi_ftp_read_response(slippi_ftp_client_t* client) {
	extern s32 top_fd;
	
	if (!client || client->socket < 0) {
		return SLIPPI_FTP_ERROR;
	}
	
	// Read response with proper multi-line handling
	int total_read = 0;
	int response_code = -1;
	char line_buffer[256];
	int line_pos = 0;
	bool is_multi_line = false;
	int lines_read = 0;
	
	dbgprintf("FTP: Starting to read response...\r\n");
	
	while (total_read < sizeof(client->response_buffer) - 1 && lines_read < 20) { // Allow more lines
		char ch;
		s32 bytes_read = recvfrom(top_fd, client->socket, &ch, 1, 0);
		
		if (bytes_read <= 0) {
			if (bytes_read < 0) {
				continue; // Retry on network error
			}
			dbgprintf("FTP: Connection closed while reading response\r\n");
			return SLIPPI_FTP_ERROR;
		}
		
		if (ch == '\n') {
			// End of line
			line_buffer[line_pos] = '\0';
			lines_read++;
			
			dbgprintf("FTP: Read line %d: %s\r\n", lines_read, line_buffer);
			
			// Copy line to response buffer
			if (total_read + line_pos < sizeof(client->response_buffer) - 1) {
				memcpy(&client->response_buffer[total_read], line_buffer, line_pos);
				client->response_buffer[total_read + line_pos] = '\n';
				total_read += line_pos + 1;
			}
			
			// Parse response code from first line
			if (response_code == -1 && line_pos >= 3) {
				response_code = (line_buffer[0] - '0') * 100 + 
								(line_buffer[1] - '0') * 10 + 
								(line_buffer[2] - '0');
				dbgprintf("FTP: Parsed response code: %d\r\n", response_code);
				
				// Check if this is a multi-line response
				if (line_pos >= 4 && line_buffer[3] == '-') {
					is_multi_line = true;
					dbgprintf("FTP: Multi-line response detected\r\n");
				}
			}
			
			// Check if we're done with the response
			if (!is_multi_line) {
				// Single line response, we're done
				dbgprintf("FTP: Single-line response complete\r\n");
				break;
			} else {
				// Multi-line response - check if this is the final line
				if (line_pos >= 3 && line_buffer[3] == ' ') {
					// Final line of multi-line response (has space after code)
					dbgprintf("FTP: Multi-line response complete\r\n");
					break;
				}
			}
			
			line_pos = 0;
		} else if (ch != '\r') {
			// Add character to line buffer
			if (line_pos < sizeof(line_buffer) - 1) {
				line_buffer[line_pos++] = ch;
			}
		}
	}
	
	client->response_buffer[total_read] = '\0';
	
	dbgprintf("FTP: Final response code: %d\r\n", response_code);
	return response_code;
}

// Transfer exact amount of data - based on ftpii's transfer_exact
static int transfer_exact(int socket, char *buf, int length, int is_send) {
	extern s32 top_fd;
	
	int result = 0;
	int remaining = length;
	int bytes_transferred;
	int timeout_attempts = 0;
	const int max_timeout_attempts = 100; // About 10 seconds of retries
	
	while (remaining > 0) {
		if (is_send) {
			bytes_transferred = sendto(top_fd, socket, buf, remaining, 0);
		} else {
			bytes_transferred = recvfrom(top_fd, socket, buf, remaining, 0);
		}
		
		if (bytes_transferred > 0) {
			remaining -= bytes_transferred;
			buf += bytes_transferred;
			timeout_attempts = 0; // Reset timeout counter on successful transfer
		} else if (bytes_transferred < 0) {
			// Network error - could be temporary, try again with timeout
			timeout_attempts++;
			if (timeout_attempts > max_timeout_attempts) {
				dbgprintf("FTP: Transfer timeout after %d attempts\r\n", timeout_attempts);
				result = -1;
				break;
			}
			// Small delay before retry
			volatile int delay_count;
			for (delay_count = 0; delay_count < 100000; delay_count++);
			continue;
		} else {
			result = -1; // Connection closed
			break;
		}
	}
	
	return result < 0 ? result : length;
}
static int slippi_ftp_send_command(slippi_ftp_client_t* client, const char* command) {
	extern s32 top_fd;
	
	if (!client || !command || client->socket < 0) {
		return SLIPPI_FTP_ERROR;
	}
	
	// Construct command with CRLF
	char cmd_buffer[256];
	int cmd_len = strlen(command);
	if (cmd_len > sizeof(cmd_buffer) - CRLF_LENGTH - 1) {
		return SLIPPI_FTP_ERROR;
	}
	
	strcpy(cmd_buffer, command);
	strcat_impl(cmd_buffer, CRLF);
	
	// Send command using transfer_exact approach from ftpii
	int total_len = cmd_len + CRLF_LENGTH;
	int result = transfer_exact(client->socket, cmd_buffer, total_len, 1);
	
	if (result < 0) {
		dbgprintf("FTP: Failed to send command: %s\r\n", command);
		return SLIPPI_FTP_ERROR;
	}
	
	dbgprintf("FTP: Sent command: %s\r\n", command);
	return SLIPPI_FTP_SUCCESS;
}

// Disconnect from FTP server - based on ftpii's cleanup approach
static void slippi_ftp_disconnect(slippi_ftp_client_t* client) {
	extern s32 top_fd;
	
	if (client->socket >= 0) {
		// Send QUIT command if still connected
		if (client->connected) {
			slippi_ftp_send_command(client, "QUIT");
			// Read response but don't check for errors since we're disconnecting anyway
			slippi_ftp_read_response(client);
		}
		
		// Close socket
		close(top_fd, client->socket);
		client->socket = -1;
	}
	
	// Reset client state
	client->connected = 0;
	client->authenticated = 0;
	memset(client->response_buffer, 0, sizeof(client->response_buffer));
}

// Clear any pending responses from the socket
static void slippi_ftp_clear_responses(slippi_ftp_client_t* client) {
	extern s32 top_fd;
	
	if (!client || client->socket < 0) {
		return;
	}
	
	// Try to read any pending data with a very short timeout
	char dummy_buffer[256];
	int i;
	for (i = 0; i < 5; i++) { // Max 5 attempts
		s32 bytes_read = recvfrom(top_fd, client->socket, dummy_buffer, sizeof(dummy_buffer), 0);
		if (bytes_read <= 0) {
			break; // No more data or error
		}
		dbgprintf("FTP: Cleared %d pending bytes\r\n", bytes_read);
	}
}

// Streaming upload functions

// Start a streaming upload
int slippi_ftp_start_stream_upload(const char* local_path, const char* remote_path) {
	extern s32 top_fd;
	
	if (!ftp_initialized || !slippi_settings || !slippi_settings->ftp_enabled) {
		dbgprintf("FTP Stream: Not initialized or FTP disabled\r\n");
		return SLIPPI_FTP_ERROR;
	}
	
	if (stream_upload.active) {
		dbgprintf("FTP Stream: Upload already active\r\n");
		return SLIPPI_FTP_ERROR;
	}
	
	// Check network status
	if (top_fd < 0) {
		dbgprintf("FTP Stream: Network not available\r\n");
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	
	dbgprintf("FTP Stream: Starting upload for %s -> %s\r\n", local_path, remote_path);
	
	// Connect to FTP server
	if (slippi_ftp_connect(&stream_client, slippi_settings->ftp_server, slippi_settings->ftp_port) != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP Stream: Connection failed\r\n");
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	
	// Authenticate
	if (slippi_ftp_authenticate(&stream_client, slippi_settings->ftp_username, slippi_settings->ftp_password) != SLIPPI_FTP_SUCCESS) {
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_AUTH_FAIL;
	}
	
	// Set binary mode
	if (slippi_ftp_send_command(&stream_client, "TYPE I") != SLIPPI_FTP_SUCCESS) {
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	int response_code = slippi_ftp_read_response(&stream_client);
	if (response_code != 200) {
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Enter passive mode
	if (slippi_ftp_send_command(&stream_client, "PASV") != SLIPPI_FTP_SUCCESS) {
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	response_code = slippi_ftp_read_response(&stream_client);
	if (response_code != 227) {
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Parse PASV response
	char* pasv_start = strstr(stream_client.response_buffer, "(");
	if (!pasv_start) {
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	int h1 = 0, h2 = 0, h3 = 0, h4 = 0, p1 = 0, p2 = 0;
	const char* p = pasv_start + 1;
	
	// Parse PASV values
	while (*p >= '0' && *p <= '9') h1 = h1 * 10 + (*p++ - '0');
	if (*p++ != ',') goto stream_pasv_error;
	while (*p >= '0' && *p <= '9') h2 = h2 * 10 + (*p++ - '0');
	if (*p++ != ',') goto stream_pasv_error;
	while (*p >= '0' && *p <= '9') h3 = h3 * 10 + (*p++ - '0');
	if (*p++ != ',') goto stream_pasv_error;
	while (*p >= '0' && *p <= '9') h4 = h4 * 10 + (*p++ - '0');
	if (*p++ != ',') goto stream_pasv_error;
	while (*p >= '0' && *p <= '9') p1 = p1 * 10 + (*p++ - '0');
	if (*p++ != ',') goto stream_pasv_error;
	while (*p >= '0' && *p <= '9') p2 = p2 * 10 + (*p++ - '0');
	if (*p != ')') goto stream_pasv_error;
	
	if (h1 > 255 || h2 > 255 || h3 > 255 || h4 > 255 || p1 > 255 || p2 > 255) goto stream_pasv_error;
	
	goto stream_pasv_success;
	
stream_pasv_error:
	slippi_ftp_disconnect(&stream_client);
	return SLIPPI_FTP_UPLOAD_FAIL;
	
stream_pasv_success:
	
	// Create data connection
	stream_upload.data_socket = socket(top_fd, AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (stream_upload.data_socket < 0) {
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Connect to data port
	struct sockaddr_in data_addr;
	memset(&data_addr, 0, sizeof(data_addr));
	data_addr.sin_len = 8;
	data_addr.sin_family = AF_INET;
	data_addr.sin_port = (p1 << 8) | p2;
	
	u32 data_ip = (h1 << 24) | (h2 << 16) | (h3 << 8) | h4;
	memcpy(&data_addr.sin_addr, &data_ip, sizeof(data_ip));
	
	if (connect(top_fd, stream_upload.data_socket, (struct sockaddr*)&data_addr) < 0) {
		close(top_fd, stream_upload.data_socket);
		stream_upload.data_socket = -1;
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Send STOR command
	char stor_cmd[256];
	_sprintf(stor_cmd, "STOR %s", remote_path);
	
	if (slippi_ftp_send_command(&stream_client, stor_cmd) != SLIPPI_FTP_SUCCESS) {
		close(top_fd, stream_upload.data_socket);
		stream_upload.data_socket = -1;
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Read initial response
	response_code = slippi_ftp_read_response(&stream_client);
	if (response_code != 150) {
		close(top_fd, stream_upload.data_socket);
		stream_upload.data_socket = -1;
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Initialize stream state
	stream_upload.active = 1;
	stream_upload.bytes_uploaded = 0;
	stream_upload.client = &stream_client;
	strncpy(stream_upload.remote_filename, remote_path, sizeof(stream_upload.remote_filename) - 1);
	strncpy(stream_upload.local_filepath, local_path, sizeof(stream_upload.local_filepath) - 1);
	
	dbgprintf("FTP Stream: Upload started successfully\r\n");
	return SLIPPI_FTP_SUCCESS;
}

// Stream data to the FTP server
int slippi_ftp_stream_data(const void* data, u32 size) {
	if (!stream_upload.active || stream_upload.data_socket < 0 || !data || size == 0) {
		return SLIPPI_FTP_ERROR;
	}
	
	int result = transfer_exact(stream_upload.data_socket, (char*)data, size, 1);
	if (result < 0) {
		dbgprintf("FTP Stream: Failed to send data chunk (%d bytes)\r\n", size);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	stream_upload.bytes_uploaded += size;
	return SLIPPI_FTP_SUCCESS;
}

// Finish the streaming upload
int slippi_ftp_finish_stream_upload(void) {
	extern s32 top_fd;
	
	if (!stream_upload.active) {
		return SLIPPI_FTP_ERROR;
	}
	
	dbgprintf("FTP Stream: Finishing upload (%d bytes uploaded)\r\n", stream_upload.bytes_uploaded);
	
	// Close data connection
	if (stream_upload.data_socket >= 0) {
		close(top_fd, stream_upload.data_socket);
		stream_upload.data_socket = -1;
	}
	
	// Read final response
	int response_code = slippi_ftp_read_response(&stream_client);
	if (response_code != 226) {
		dbgprintf("FTP Stream: Transfer completion failed with code %d\r\n", response_code);
		slippi_ftp_disconnect(&stream_client);
		stream_upload.active = 0;
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Disconnect from FTP server
	slippi_ftp_disconnect(&stream_client);
	stream_upload.active = 0;
	
	dbgprintf("FTP Stream: Upload completed successfully\r\n");
	return SLIPPI_FTP_SUCCESS;
}

// Cancel the streaming upload
void slippi_ftp_cancel_stream_upload(void) {
	extern s32 top_fd;
	
	if (!stream_upload.active) {
		return;
	}
	
	dbgprintf("FTP Stream: Cancelling active upload\r\n");
	
	if (stream_upload.data_socket >= 0) {
		close(top_fd, stream_upload.data_socket);
		stream_upload.data_socket = -1;
	}
	
	slippi_ftp_disconnect(&stream_client);
	stream_upload.active = 0;
}

// Check if streaming upload is active
int slippi_ftp_is_stream_active(void) {
	return stream_upload.active;
}
