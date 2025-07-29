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
#include <stdio.h>
#include <errno.h>

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

// Function prototypes for internal functions
static int slippi_ftp_connect(slippi_ftp_client_t* client, const char* server, unsigned short port);
static int slippi_ftp_authenticate(slippi_ftp_client_t* client, const char* username, const char* password);
static int slippi_ftp_upload_file(slippi_ftp_client_t* client, const char* local_path, const char* remote_path);
static int slippi_ftp_read_response(slippi_ftp_client_t* client);
static int slippi_ftp_send_command(slippi_ftp_client_t* client, const char* command);
static void slippi_ftp_disconnect(slippi_ftp_client_t* client);
static int transfer_exact(int socket, char *buf, int length, int is_send);
static int send_from_file(int data_socket, const char* filepath);

// Initialize FTP client system
int slippi_ftp_init(void) {
	if (ftp_initialized) {
		return SLIPPI_FTP_SUCCESS;
	}
	
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
	
	upload_in_progress = 0;
	ftp_initialized = 1;
	
	return SLIPPI_FTP_SUCCESS;
}

// Cleanup FTP client system
void slippi_ftp_cleanup(void) {
	if (!ftp_initialized) {
		return;
	}
	
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
	if (!ftp_initialized) {
		slippi_ftp_init();
	}
	
	if (!filepath || queue_count >= SLIPPI_FTP_QUEUE_SIZE) {
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
		return SLIPPI_FTP_ERROR;
	}
	
	// Check if FTP is enabled in config
	if (!slippi_settings || !slippi_settings->ftp_enabled) {
		return SLIPPI_FTP_ERROR;
	}
	
	upload_in_progress = 1;
	
	// Connect to FTP server
	if (slippi_ftp_connect(&ftp_client, slippi_settings->ftp_server, slippi_settings->ftp_port) != SLIPPI_FTP_SUCCESS) {
		upload_in_progress = 0;
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
	while (queue_count > 0) {
		slippi_ftp_queue_entry_t* entry = &ftp_queue[queue_head];
		
		if (entry->queued) {
			// Construct remote path
			char remote_path[256];
			if (strlen(slippi_settings->ftp_directory) > 0) {
				_sprintf(remote_path, "%s/%s", slippi_settings->ftp_directory, entry->filename);
			} else {
				strncpy(remote_path, entry->filename, sizeof(remote_path) - 1);
			}
			
			// Upload file
			if (slippi_ftp_upload_file(&ftp_client, entry->filepath, remote_path) == SLIPPI_FTP_SUCCESS) {
				uploaded++;
			}
		}
		
		// Remove from queue
		entry->queued = 0;
		queue_head = (queue_head + 1) % SLIPPI_FTP_QUEUE_SIZE;
		queue_count--;
	}
	
	slippi_ftp_disconnect(&ftp_client);
	upload_in_progress = 0;
	
	return uploaded > 0 ? SLIPPI_FTP_SUCCESS : SLIPPI_FTP_UPLOAD_FAIL;
}

// Cancel any uploads in progress
void slippi_ftp_cancel_uploads(void) {
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
		return SLIPPI_FTP_ERROR;
	}
	
	// Create socket
	client->socket = socket(top_fd, AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (client->socket < 0) {
		dbgprintf("FTP: Failed to create socket\r\n");
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	
	// Set up server address
	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_len = 8;
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = port;
	
	// Parse IP address - ftpii uses inet_aton but we'll do manual parsing
	u32 ip_addr = 0;
	int h1, h2, h3, h4;
	if (sscanf(server, "%d.%d.%d.%d", &h1, &h2, &h3, &h4) == 4) {
		ip_addr = (h1 << 24) | (h2 << 16) | (h3 << 8) | h4;
		memcpy(&server_addr.sin_addr, &ip_addr, sizeof(ip_addr));
	} else {
		dbgprintf("FTP: Invalid IP address format\r\n");
		close(top_fd, client->socket);
		client->socket = -1;
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	
	// Connect to server
	if (connect(top_fd, client->socket, (struct sockaddr*)&server_addr) < 0) {
		dbgprintf("FTP: Failed to connect to server\r\n");
		close(top_fd, client->socket);
		client->socket = -1;
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	
	client->connected = 1;
	
	// Read welcome message (220)
	int response_code = slippi_ftp_read_response(client);
	if (response_code != 220) {
		dbgprintf("FTP: Invalid welcome message: %d\r\n", response_code);
		slippi_ftp_disconnect(client);
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	
	return SLIPPI_FTP_SUCCESS;
}

// Send file data over socket - based on ftpii's send_from_file
static int send_from_file(int data_socket, const char* filepath) {
	FIL file;
	FRESULT result = f_open_char(&file, filepath, FA_READ);
	if (result != FR_OK) {
		dbgprintf("FTP: Failed to open file for upload: %s\r\n", filepath);
		return -1;
	}
	
	char buffer[1024]; // Similar to ftpii's FREAD_BUFFER_SIZE
	UINT bytes_read;
	int total_sent = 0;
	int send_result = 0;
	
	while (f_read(&file, buffer, sizeof(buffer), &bytes_read) == FR_OK && bytes_read > 0) {
		send_result = transfer_exact(data_socket, buffer, bytes_read, 1);
		if (send_result < 0) {
			dbgprintf("FTP: Failed to send file data\r\n");
			break;
		}
		total_sent += bytes_read;
		
		// Check if we read less than buffer size (end of file)
		if (bytes_read < sizeof(buffer)) {
			send_result = 0; // Success
			break;
		}
	}
	
	f_close(&file);
	
	if (send_result >= 0) {
		dbgprintf("FTP: Successfully sent %d bytes\r\n", total_sent);
		return 0;
	} else {
		return send_result;
	}
}
static int slippi_ftp_authenticate(slippi_ftp_client_t* client, const char* username, const char* password) {
	if (!client->connected) {
		return SLIPPI_FTP_ERROR;
	}
	
	// Send USER command
	char user_cmd[128];
	_sprintf(user_cmd, "USER %s", username);
	
	if (slippi_ftp_send_command(client, user_cmd) != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send USER command\r\n");
		return SLIPPI_FTP_AUTH_FAIL;
	}
	
	// Read response to USER command (expect 331 "User name okay, need password")
	int response_code = slippi_ftp_read_response(client);
	if (response_code != 331) {
		dbgprintf("FTP: USER command failed with code %d\r\n", response_code);
		return SLIPPI_FTP_AUTH_FAIL;
	}
	
	// Send PASS command
	char pass_cmd[128];
	_sprintf(pass_cmd, "PASS %s", password);
	
	if (slippi_ftp_send_command(client, pass_cmd) != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send PASS command\r\n");
		return SLIPPI_FTP_AUTH_FAIL;
	}
	
	// Read response to PASS command (expect 230 "User logged in, proceed")
	response_code = slippi_ftp_read_response(client);
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
		return SLIPPI_FTP_ERROR;
	}
	
	// Set binary mode (TYPE I)
	if (slippi_ftp_send_command(client, "TYPE I") != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send TYPE command\r\n");
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	int response_code = slippi_ftp_read_response(client);
	if (response_code != 200) {
		dbgprintf("FTP: TYPE command failed with code %d\r\n", response_code);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Enter passive mode (PASV)
	if (slippi_ftp_send_command(client, "PASV") != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send PASV command\r\n");
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	response_code = slippi_ftp_read_response(client);
	if (response_code != 227) {
		dbgprintf("FTP: PASV command failed with code %d\r\n", response_code);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Parse PASV response "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)"
	char* pasv_start = strstr(client->response_buffer, "(");
	if (!pasv_start) {
		dbgprintf("FTP: Invalid PASV response format\r\n");
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	int h1, h2, h3, h4, p1, p2;
	if (sscanf(pasv_start + 1, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
		dbgprintf("FTP: Failed to parse PASV response\r\n");
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Create data connection socket
	s32 data_socket = socket(top_fd, AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (data_socket < 0) {
		dbgprintf("FTP: Failed to create data socket\r\n");
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Set up data connection address
	struct sockaddr_in data_addr;
	memset(&data_addr, 0, sizeof(data_addr));
	data_addr.sin_len = 8;
	data_addr.sin_family = AF_INET;
	data_addr.sin_port = (p1 << 8) | p2;
	
	u32 data_ip = (h1 << 24) | (h2 << 16) | (h3 << 8) | h4;
	memcpy(&data_addr.sin_addr, &data_ip, sizeof(data_ip));
	
	// Connect to data port
	if (connect(top_fd, data_socket, (struct sockaddr*)&data_addr) < 0) {
		dbgprintf("FTP: Failed to connect to data port\r\n");
		close(top_fd, data_socket);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Send STOR command
	char stor_cmd[256];
	_sprintf(stor_cmd, "STOR %s", remote_path);
	
	if (slippi_ftp_send_command(client, stor_cmd) != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send STOR command\r\n");
		close(top_fd, data_socket);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Read initial response (should be 150 "Opening data connection")
	response_code = slippi_ftp_read_response(client);
	if (response_code != 150) {
		dbgprintf("FTP: STOR command failed with code %d\r\n", response_code);
		close(top_fd, data_socket);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Transfer file data
	int file_result = send_from_file(data_socket, local_path);
	
	// Close data connection
	close(top_fd, data_socket);
	
	if (file_result < 0) {
		dbgprintf("FTP: File transfer failed\r\n");
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Read final response (should be 226 "Transfer complete")
	response_code = slippi_ftp_read_response(client);
	if (response_code != 226) {
		dbgprintf("FTP: Transfer completion failed with code %d\r\n", response_code);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	dbgprintf("FTP: Successfully uploaded %s\r\n", remote_path);
	return SLIPPI_FTP_SUCCESS;
}

// Read response from FTP server - based on ftpii's approach
static int slippi_ftp_read_response(slippi_ftp_client_t* client) {
	extern s32 top_fd;
	
	if (!client || client->socket < 0) {
		return SLIPPI_FTP_ERROR;
	}
	
	// Read response line by line
	int total_read = 0;
	int response_code = -1;
	char line_buffer[256];
	int line_pos = 0;
	bool multi_line = false;
	
	while (total_read < sizeof(client->response_buffer) - 1) {
		char ch;
		s32 bytes_read = recvfrom(top_fd, client->socket, &ch, 1, 0);
		
		if (bytes_read <= 0) {
			if (bytes_read < 0) {
				// Network error - could be temporary, try again  
				continue;
			}
			dbgprintf("FTP: Failed to read response\r\n");
			return SLIPPI_FTP_ERROR;
		}
		
		if (ch == '\n') {
			// End of line
			line_buffer[line_pos] = '\0';
			
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
				
				// Check if this is a multi-line response
				if (line_pos >= 4 && line_buffer[3] == '-') {
					multi_line = true;
				}
			}
			
			// Check if we're done
			if (!multi_line || (line_pos >= 3 && line_buffer[3] != '-')) {
				break;
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
	
	dbgprintf("FTP: Server response (%d): %s\r\n", response_code, client->response_buffer);
	return response_code;
}

// Transfer exact amount of data - based on ftpii's transfer_exact
static int transfer_exact(int socket, char *buf, int length, int is_send) {
	extern s32 top_fd;
	
	int result = 0;
	int remaining = length;
	int bytes_transferred;
	
	while (remaining > 0) {
		if (is_send) {
			bytes_transferred = sendto(top_fd, socket, buf, remaining, 0);
		} else {
			bytes_transferred = recvfrom(top_fd, socket, buf, remaining, 0);
		}
		
		if (bytes_transferred > 0) {
			remaining -= bytes_transferred;
			buf += bytes_transferred;
		} else if (bytes_transferred < 0) {
			// Network error - could be temporary, try again  
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
