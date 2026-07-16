/*
Slippi FTP Client for Nintendont
Full FTP client for uploading replay files
Based on ftpii FTP implementation
*/

#include "SlippiFTP.h"
#include "Config.h"
#include "SlippiMemory.h"
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

// Shared memory for CMD 0xA0xx controller metadata (ARM physical)
#define SFW_EXTRA_DATA_ADDR  0x13080020
#define SFW_XDATA_STRIDE     0x02B0
#define SFW_XOFF_NCHUNKS     0x08
#define SFW_XOFF_CHUNKS      0x10
#define SFW_CHUNK_SIZE       80
#define SFW_CHUNK_DATA       78

#define FTP_METADATA_PLAYER_BUF 1024
#define FTP_METADATA_UBJ_BUF 2300
#define FTP_METADATA_HEX_BUF (FTP_METADATA_UBJ_BUF * 2 + 1)
#define FTP_SITE_CMD_BUF (FTP_METADATA_HEX_BUF + 32)

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
static int ftp_initialized = 0;

// Streaming upload state
static slippi_ftp_stream_t stream_upload;
static slippi_ftp_client_t stream_client;
// Large SITE metadata commands can exceed 256 bytes; keep command buffer in BSS.
static char ftp_command_buffer[FTP_SITE_CMD_BUF + CRLF_LENGTH + 1];

// Function prototypes for internal functions
static int slippi_ftp_connect(slippi_ftp_client_t* client, const char* server, unsigned short port);
static int slippi_ftp_authenticate(slippi_ftp_client_t* client, const char* username, const char* password);
static int slippi_ftp_upload_file(slippi_ftp_client_t* client, const char* local_path, const char* remote_path);
static int slippi_ftp_read_response(slippi_ftp_client_t* client);
static int slippi_ftp_send_command(slippi_ftp_client_t* client, const char* command);
static void slippi_ftp_disconnect(slippi_ftp_client_t* client);
static int transfer_exact(int socket, char *buf, int length, int is_send);
static int send_from_file(int data_socket, const char* filepath);
static int slippi_ftp_send_stream_metadata_ubjson(slippi_ftp_client_t* client);
static u16 reassembleControllerMetadata(u32 chanBase, u8 *dest, u16 maxLen);
static u16 validateUbjsonStringDict(const u8 *buf, u16 len);
static int build_stream_metadata_ubjson(u8* out, u16 out_len);
static void bytes_to_hex(const u8* src, u16 len, char* dest);

// Initialize FTP client system
int slippi_ftp_init(void) {
	dbgprintf("FTP: Starting initialization...\r\n");
	
	if (ftp_initialized) {
		dbgprintf("FTP: Already initialized\r\n");
		return SLIPPI_FTP_SUCCESS;
	}
	
	dbgprintf("FTP: Clearing streaming state...\r\n");
	
	// Clear streaming state
	memset(&stream_upload, 0, sizeof(stream_upload));
	stream_upload.data_socket = -1;
	memset(&stream_client, 0, sizeof(stream_client));
	stream_client.socket = -1;
	
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
	
	ftp_initialized = 0;
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

/* Validate UBJSON object containing only string key/value pairs.
 * Returns validated byte count (including { and }), or 0 on failure. */
static u16 validateUbjsonStringDict(const u8 *buf, u16 len)
{
	if (len < 2 || buf[0] != '{')
		return 0;

	u16 pos = 1;
	while (pos < len && buf[pos] != '}')
	{
		u16 i;
		if (pos + 2 > len || (buf[pos] != 'U' && buf[pos] != 'i'))
			return 0;
		u8 keyLen = buf[pos + 1];
		pos += 2;
		if (keyLen == 0 || pos + keyLen > len)
			return 0;
		for (i = 0; i < keyLen; i++)
			if (buf[pos + i] < 0x20 || buf[pos + i] > 0x7E)
				return 0;
		pos += keyLen;

		if (pos + 3 > len || buf[pos] != 'S' ||
		    (buf[pos + 1] != 'U' && buf[pos + 1] != 'i'))
			return 0;
		u8 valLen = buf[pos + 2];
		pos += 3;
		if (pos + valLen > len)
			return 0;
		for (i = 0; i < valLen; i++)
			if (buf[pos + i] < 0x20 || buf[pos + i] > 0x7E)
				return 0;
		pos += valLen;
	}

	if (pos >= len || buf[pos] != '}')
		return 0;

	return pos + 1;
}

/* Reassemble data from raw 80-byte SI chunks into contiguous buffer. */
static u16 reassembleControllerMetadata(u32 chanBase, u8 *dest, u16 maxLen)
{
	u32 nChunks  = read32(chanBase + SFW_XOFF_NCHUNKS);

	if (nChunks == 0 || nChunks > 8)
		return 0;

	u16 written = 0;
	u32 i;
	for (i = 0; i < nChunks && written < maxLen; i++)
	{
		u8 *chunkN = (u8*)(chanBase + SFW_XOFF_CHUNKS + i * SFW_CHUNK_SIZE);
		u16 copyLen = SFW_CHUNK_DATA;
		if (written + copyLen > maxLen)
			copyLen = maxLen - written;
		memcpy(dest + written, chunkN + 2, copyLen);
		written += copyLen;
	}

	return written;
}

static int build_stream_metadata_ubjson(u8* out, u16 out_len)
{
	u16 writePos = 0;
	int ch;
	static u8 playerBuf[FTP_METADATA_PLAYER_BUF];

	if (!out || out_len < 2)
		return 0;

	out[writePos++] = '{';

	/* Ensure coherent read from shared metadata memory. */
	sync_before_read((void*)SFW_EXTRA_DATA_ADDR, 4 * SFW_XDATA_STRIDE);

	for (ch = 0; ch < 4; ch++)
	{
		u32 addr = SFW_EXTRA_DATA_ADDR + ch * SFW_XDATA_STRIDE;
		u32 calls = read32(addr + 0x04);
		if (calls == 0)
			continue;

		u32 tag = read32(addr + 0x00);
		if (tag != 0xCA110000)
			continue;

		u16 dataLen = reassembleControllerMetadata(addr, playerBuf, sizeof(playerBuf));
		u16 validLen = validateUbjsonStringDict(playerBuf, dataLen);
		if (validLen == 0)
			continue;

		if (writePos + 3 + validLen + 1 > out_len)
			break;

		out[writePos++] = 'U';
		out[writePos++] = 1;
		out[writePos++] = '0' + ch;
		memcpy(&out[writePos], playerBuf, validLen);
		writePos += validLen;
	}

	out[writePos++] = '}';
	return writePos;
}

static void bytes_to_hex(const u8* src, u16 len, char* dest)
{
	static const char hexdigits[] = "0123456789abcdef";
	u16 i;
	for (i = 0; i < len; i++)
	{
		dest[i * 2] = hexdigits[(src[i] >> 4) & 0x0F];
		dest[i * 2 + 1] = hexdigits[src[i] & 0x0F];
	}
	dest[len * 2] = '\0';
}

static int slippi_ftp_send_stream_metadata_ubjson(slippi_ftp_client_t* client)
{
	if (!client || !client->connected || !client->authenticated)
		return SLIPPI_FTP_ERROR;

	u8 ubjPayload[FTP_METADATA_UBJ_BUF];
	char hexPayload[FTP_METADATA_HEX_BUF];
	char siteCmd[FTP_SITE_CMD_BUF];

	int ubjLen = build_stream_metadata_ubjson(ubjPayload, sizeof(ubjPayload));
	if (ubjLen <= 2)
	{
		dbgprintf("FTP: No controller metadata available for SITE SLPMETAUBJ\r\n");
		return SLIPPI_FTP_SUCCESS;
	}

	bytes_to_hex(ubjPayload, (u16)ubjLen, hexPayload);
	_sprintf(siteCmd, "SITE SLPMETAUBJ %s", hexPayload);

	int send_result = slippi_ftp_send_command(client, siteCmd);
	if (send_result != SLIPPI_FTP_SUCCESS)
		return send_result;

	int response_code = slippi_ftp_read_response(client);
	if (response_code == SLIPPI_FTP_ERROR || response_code == SLIPPI_FTP_CONNECT_FAIL)
		return SLIPPI_FTP_CONNECT_FAIL;
	if (response_code != 200)
	{
		dbgprintf("FTP: SITE SLPMETAUBJ failed with code %d\r\n", response_code);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}

	dbgprintf("FTP: Applied SITE SLPMETAUBJ metadata override\r\n");
	return SLIPPI_FTP_SUCCESS;
}
// Authenticate with FTP server
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
	int send_result = slippi_ftp_send_command(client, user_cmd);
	if (send_result != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send USER command\r\n");
		if (send_result == SLIPPI_FTP_CONNECT_FAIL) {
			return SLIPPI_FTP_CONNECT_FAIL;
		}
		return SLIPPI_FTP_AUTH_FAIL;
	}
	
	// Read response to USER command (expect 331 "User name okay, need password" or 230 "User logged in")
	dbgprintf("FTP: Reading USER response...\r\n");
	int response_code = slippi_ftp_read_response(client);
	dbgprintf("FTP: USER response code: %d\r\n", response_code);
	
	if (response_code == SLIPPI_FTP_ERROR || response_code == SLIPPI_FTP_CONNECT_FAIL) {
		dbgprintf("FTP: Lost connection during USER command\r\n");
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	
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
	send_result = slippi_ftp_send_command(client, pass_cmd);
	if (send_result != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send PASS command\r\n");
		if (send_result == SLIPPI_FTP_CONNECT_FAIL) {
			return SLIPPI_FTP_CONNECT_FAIL;
		}
		return SLIPPI_FTP_AUTH_FAIL;
	}
	
	// Read response to PASS command (expect 230 "User logged in, proceed")
	dbgprintf("FTP: Reading PASS response...\r\n");
	response_code = slippi_ftp_read_response(client);
	dbgprintf("FTP: PASS response code: %d\r\n", response_code);
	
	if (response_code == SLIPPI_FTP_ERROR || response_code == SLIPPI_FTP_CONNECT_FAIL) {
		dbgprintf("FTP: Lost connection during PASS command\r\n");
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	
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
	int send_result = slippi_ftp_send_command(client, "TYPE I");
	if (send_result != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send TYPE command\r\n");
		if (send_result == SLIPPI_FTP_CONNECT_FAIL) {
			return SLIPPI_FTP_CONNECT_FAIL;
		}
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	dbgprintf("FTP: Reading TYPE response\r\n");
	int response_code = slippi_ftp_read_response(client);
	dbgprintf("FTP: TYPE response code: %d\r\n", response_code);
	if (response_code == SLIPPI_FTP_ERROR || response_code == SLIPPI_FTP_CONNECT_FAIL) {
		dbgprintf("FTP: Lost connection during TYPE command\r\n");
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	if (response_code != 200) {
		dbgprintf("FTP: TYPE command failed with code %d\r\n", response_code);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Enter passive mode (PASV)
	dbgprintf("FTP: Sending PASV command\r\n");
	send_result = slippi_ftp_send_command(client, "PASV");
	if (send_result != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send PASV command\r\n");
		if (send_result == SLIPPI_FTP_CONNECT_FAIL) {
			return SLIPPI_FTP_CONNECT_FAIL;
		}
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	dbgprintf("FTP: Reading PASV response\r\n");
	response_code = slippi_ftp_read_response(client);
	dbgprintf("FTP: PASV response code: %d\r\n", response_code);
	if (response_code == SLIPPI_FTP_ERROR || response_code == SLIPPI_FTP_CONNECT_FAIL) {
		dbgprintf("FTP: Lost connection during PASV command\r\n");
		return SLIPPI_FTP_CONNECT_FAIL;
	}
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
	send_result = slippi_ftp_send_command(client, stor_cmd);
	if (send_result != SLIPPI_FTP_SUCCESS) {
		dbgprintf("FTP: Failed to send STOR command\r\n");
		close(top_fd, data_socket);
		if (send_result == SLIPPI_FTP_CONNECT_FAIL) {
			return SLIPPI_FTP_CONNECT_FAIL;
		}
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Read initial response (should be 150 "Opening data connection")
	dbgprintf("FTP: Reading STOR response\r\n");
	response_code = slippi_ftp_read_response(client);
	dbgprintf("FTP: STOR response code: %d\r\n", response_code);
	if (response_code == SLIPPI_FTP_ERROR || response_code == SLIPPI_FTP_CONNECT_FAIL) {
		dbgprintf("FTP: Lost connection during STOR command\r\n");
		close(top_fd, data_socket);
		return SLIPPI_FTP_CONNECT_FAIL;
	}
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
	if (response_code == SLIPPI_FTP_ERROR || response_code == SLIPPI_FTP_CONNECT_FAIL) {
		dbgprintf("FTP: Lost connection during final transfer response\r\n");
		return SLIPPI_FTP_CONNECT_FAIL;
	}
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
	
	// Check if we're still connected
	if (!client->connected) {
		dbgprintf("FTP: Client not connected, aborting response read\r\n");
		return SLIPPI_FTP_ERROR;
	}
	
	// Read response with proper multi-line handling
	int total_read = 0;
	int response_code = -1;
	char line_buffer[256];
	int line_pos = 0;
	bool is_multi_line = false;
	int lines_read = 0;
	int consecutive_failures = 0;
	const int max_consecutive_failures = 10;
	
	dbgprintf("FTP: Starting to read response...\r\n");
	
	while (total_read < sizeof(client->response_buffer) - 1 && lines_read < 20) { // Allow more lines
		char ch;
		s32 bytes_read = recvfrom(top_fd, client->socket, &ch, 1, 0);
		
		if (bytes_read <= 0) {
			consecutive_failures++;
			if (bytes_read == 0) {
				// Connection closed by server
				dbgprintf("FTP: Server closed connection during response read\r\n");
				client->connected = 0;
				return SLIPPI_FTP_CONNECT_FAIL;
			} else if (bytes_read < 0) {
				// Network error
				if (consecutive_failures >= max_consecutive_failures) {
					dbgprintf("FTP: Too many consecutive network errors (%d), assuming disconnection\r\n", consecutive_failures);
					client->connected = 0;
					return SLIPPI_FTP_CONNECT_FAIL;
				}
				// Small delay before retry
				volatile int delay_count;
				for (delay_count = 0; delay_count < 50000; delay_count++);
				continue;
			}
		} else {
			consecutive_failures = 0; // Reset on successful read
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
	int connection_failures = 0;
	const int max_timeout_attempts = 100; // About 10 seconds of retries
	const int max_connection_failures = 5; // Allow a few connection failures before giving up
	
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
			connection_failures = 0; // Reset connection failure counter
		} else if (bytes_transferred == 0) {
			// Connection closed by peer
			connection_failures++;
			dbgprintf("FTP: Connection closed by peer during transfer (attempt %d/%d)\r\n", 
				connection_failures, max_connection_failures);
			if (connection_failures >= max_connection_failures) {
				dbgprintf("FTP: Too many connection failures, aborting transfer\r\n");
				result = -1;
				break;
			}
			// Small delay before giving up
			volatile int delay_count;
			for (delay_count = 0; delay_count < 200000; delay_count++);
			result = -1;
			break;
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
		}
	}
	
	return result < 0 ? result : length;
}
static int slippi_ftp_send_command(slippi_ftp_client_t* client, const char* command) {
	extern s32 top_fd;
	
	if (!client || !command || client->socket < 0) {
		return SLIPPI_FTP_ERROR;
	}
	
	// Check if we're still connected before sending
	if (!client->connected) {
		dbgprintf("FTP: Client not connected, cannot send command: %s\r\n", command);
		return SLIPPI_FTP_ERROR;
	}
	
	// Construct command with CRLF
	int cmd_len = strlen(command);
	if (cmd_len > sizeof(ftp_command_buffer) - CRLF_LENGTH - 1) {
		dbgprintf("FTP: Command too long (%d bytes): %s\r\n", cmd_len, command);
		return SLIPPI_FTP_ERROR;
	}
	
	strcpy(ftp_command_buffer, command);
	strcat_impl(ftp_command_buffer, CRLF);
	
	// Send command using transfer_exact approach from ftpii
	int total_len = cmd_len + CRLF_LENGTH;
	int result = transfer_exact(client->socket, ftp_command_buffer, total_len, 1);
	
	if (result < 0) {
		dbgprintf("FTP: Failed to send command: %s (connection may be lost)\r\n", command);
		// Mark connection as lost
		client->connected = 0;
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	
	dbgprintf("FTP: Sent command: %s\r\n", command);
	return SLIPPI_FTP_SUCCESS;
}

// Disconnect from FTP server - based on ftpii's cleanup approach
static void slippi_ftp_disconnect(slippi_ftp_client_t* client) {
	extern s32 top_fd;
	
	if (!client) {
		return;
	}
	
	if (client->socket >= 0) {
		// Send QUIT command if still connected, but don't fail if it doesn't work
		if (client->connected) {
			dbgprintf("FTP: Sending QUIT command before disconnect\r\n");
			// Don't check result - connection might already be lost
			slippi_ftp_send_command(client, "QUIT");
			// Try to read response but ignore errors
			slippi_ftp_read_response(client);
		}
		
		// Close socket
		dbgprintf("FTP: Closing socket %d\r\n", client->socket);
		close(top_fd, client->socket);
		client->socket = -1;
	}
	
	// Reset client state
	client->connected = 0;
	client->authenticated = 0;
	memset(client->response_buffer, 0, sizeof(client->response_buffer));
	dbgprintf("FTP: Disconnect complete\r\n");
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

	// Send player metadata override (UBJSON object encoded as hex) if present.
	int meta_result = slippi_ftp_send_stream_metadata_ubjson(&stream_client);
	if (meta_result != SLIPPI_FTP_SUCCESS) {
		slippi_ftp_disconnect(&stream_client);
		if (meta_result == SLIPPI_FTP_CONNECT_FAIL) {
			return SLIPPI_FTP_CONNECT_FAIL;
		}
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Set binary mode
	int send_result = slippi_ftp_send_command(&stream_client, "TYPE I");
	if (send_result != SLIPPI_FTP_SUCCESS) {
		slippi_ftp_disconnect(&stream_client);
		if (send_result == SLIPPI_FTP_CONNECT_FAIL) {
			return SLIPPI_FTP_CONNECT_FAIL;
		}
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	int response_code = slippi_ftp_read_response(&stream_client);
	if (response_code == SLIPPI_FTP_ERROR || response_code == SLIPPI_FTP_CONNECT_FAIL) {
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_CONNECT_FAIL;
	}
	if (response_code != 200) {
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Enter passive mode
	send_result = slippi_ftp_send_command(&stream_client, "PASV");
	if (send_result != SLIPPI_FTP_SUCCESS) {
		slippi_ftp_disconnect(&stream_client);
		if (send_result == SLIPPI_FTP_CONNECT_FAIL) {
			return SLIPPI_FTP_CONNECT_FAIL;
		}
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	response_code = slippi_ftp_read_response(&stream_client);
	if (response_code == SLIPPI_FTP_ERROR || response_code == SLIPPI_FTP_CONNECT_FAIL) {
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_CONNECT_FAIL;
	}
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
	
	send_result = slippi_ftp_send_command(&stream_client, stor_cmd);
	if (send_result != SLIPPI_FTP_SUCCESS) {
		close(top_fd, stream_upload.data_socket);
		stream_upload.data_socket = -1;
		slippi_ftp_disconnect(&stream_client);
		if (send_result == SLIPPI_FTP_CONNECT_FAIL) {
			return SLIPPI_FTP_CONNECT_FAIL;
		}
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	// Read initial response
	response_code = slippi_ftp_read_response(&stream_client);
	if (response_code == SLIPPI_FTP_ERROR || response_code == SLIPPI_FTP_CONNECT_FAIL) {
		close(top_fd, stream_upload.data_socket);
		stream_upload.data_socket = -1;
		slippi_ftp_disconnect(&stream_client);
		return SLIPPI_FTP_CONNECT_FAIL;
	}
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
		dbgprintf("FTP Stream: Failed to send data chunk (%d bytes), marking stream as inactive\r\n", size);
		// Mark stream as inactive but don't clean up yet - let finish/cancel handle cleanup
		stream_upload.active = 0;
		return SLIPPI_FTP_UPLOAD_FAIL;
	}
	
	stream_upload.bytes_uploaded += size;
	return SLIPPI_FTP_SUCCESS;
}

// Finish the streaming upload
int slippi_ftp_finish_stream_upload(void) {
	extern s32 top_fd;
	
	if (!stream_upload.active && stream_upload.data_socket < 0) {
		// Already finished or never started
		return SLIPPI_FTP_ERROR;
	}
	
	dbgprintf("FTP Stream: Finishing upload (%d bytes uploaded)\r\n", stream_upload.bytes_uploaded);
	
	// Close data connection
	if (stream_upload.data_socket >= 0) {
		close(top_fd, stream_upload.data_socket);
		stream_upload.data_socket = -1;
	}
	
	// Only try to read response if we still think we're connected
	if (stream_client.connected) {
		// Read final response
		int response_code = slippi_ftp_read_response(&stream_client);
		if (response_code == SLIPPI_FTP_ERROR || response_code == SLIPPI_FTP_CONNECT_FAIL) {
			dbgprintf("FTP Stream: Lost connection while finishing upload\r\n");
			slippi_ftp_disconnect(&stream_client);
			stream_upload.active = 0;
			return SLIPPI_FTP_CONNECT_FAIL;
		}
		if (response_code != 226) {
			dbgprintf("FTP Stream: Transfer completion failed with code %d\r\n", response_code);
			slippi_ftp_disconnect(&stream_client);
			stream_upload.active = 0;
			return SLIPPI_FTP_UPLOAD_FAIL;
		}
	} else {
		dbgprintf("FTP Stream: Connection already lost, skipping final response read\r\n");
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
