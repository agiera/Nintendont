#ifndef _SLIPPI_H
#define _SLIPPI_H

/* These header files *MUST* match across these projects:
 *
 *	project-slippi/slippi-wiiconf/source/Slippi.h
 *	project-slippi/Nintendont/common/include/Slippi.h
 *
 * If you update one, make sure you update the other!
 */

#define SLIPPI_DAT_FILE		"/slippi_console.dat"
#define SD_SLIPPI_DAT_FILE	"sd:/slippi_console.dat"
#define USB_SLIPPI_DAT_FILE	"usb:/slippi_console.dat"

/* struct slippi_settings
 * rtc_bias - 32-bits, "difference between RTC and local time" in seconds.
 *	      Specifically, the number of seconds since 1/1/2000 00:00:00.
 * nickname - 32 byte, user-configurable nickname for this console.
 *
 * If the RTC bias is set to zero, we can assume that this is irrelevant or
 * un-set (meaning, we can use the bias from SYSCONF NAND flash).
 *
 * If the first byte of nickname[32] is NUL (0x00), we can assume that the
 * nickname is un-set (meaning, we should use the nickname from SYSCONF).
 */

struct slippi_settings {

	// 32-bit, "difference between RTC and local time" in seconds.
	// This is the number of seconds since 1/1/2000 00:00:00.
	unsigned int rtc_bias;

	// 32-byte, user-configurable nickname for this console.
	// This nickname will be written to replay file metadata.
	char nickname[32];

	// FTP upload settings
	unsigned int ftp_enabled;		// 1 to enable FTP upload, 0 to disable
	char ftp_server[64];			// FTP server hostname/IP
	unsigned short ftp_port;		// FTP server port (typically 21)
	char ftp_username[32];			// FTP username
	char ftp_password[32];			// FTP password
	char ftp_directory[64];			// Upload directory on server
};

#endif // _SLIPPI_H
