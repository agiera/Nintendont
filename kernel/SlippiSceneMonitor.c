/*
Slippi Scene Monitor for Nintendont
Monitors game scenes to trigger FTP uploads at appropriate times
*/

#include "SlippiSceneMonitor.h"
#include "SlippiFTP.h"
#include "SlippiDebug.h"
#include "Config.h"
#include "EXI.h"
#include "string.h"

// Global scene monitor state
static slippi_scene_monitor_t scene_monitor;
static int monitor_initialized = 0;

// Initialize scene monitor
int slippi_scene_monitor_init(void) {
	if (monitor_initialized) {
		return 0;
	}
	
	memset(&scene_monitor, 0, sizeof(scene_monitor));
	scene_monitor.current_major_scene = 0xFF; // Invalid scene to force initial update
	scene_monitor.current_minor_scene = 0xFF;
	scene_monitor.previous_major_scene = 0xFF;
	scene_monitor.previous_minor_scene = 0xFF;
	scene_monitor.uploads_enabled = 1;
	scene_monitor.upload_timer = 0;
	
	monitor_initialized = 1;
	return 0;
}

// Handle scene transitions
static void slippi_scene_handle_transition(void) {
	// Check if we just entered character select screen
	// This happens in VS Mode (0x02) with minor scene CSS (0x00)
	// or VS Online (0x08) with minor scene CSS (0x00)
	int is_now_css = (scene_monitor.current_major_scene == SCENE_VS_MODE && scene_monitor.current_minor_scene == SCENE_VS_CSS);
	
	int was_css = (scene_monitor.previous_major_scene == SCENE_VS_MODE && scene_monitor.previous_minor_scene == SCENE_VS_CSS);
	
	if (is_now_css && !was_css) {
		// Start upload timer (wait a few seconds before starting upload)
		scene_monitor.upload_timer = 30; // 3 seconds at 10Hz update rate
		scene_monitor.uploads_enabled = 1;
	}
	
	// Check if we left character select for stage select
	// This happens when going from CSS to SSS
	int is_now_sss = (scene_monitor.current_major_scene == SCENE_VS_MODE && scene_monitor.current_minor_scene == SCENE_VS_SSS);
	
	if (was_css && is_now_sss) {
		if (slippi_settings && slippi_settings->ftp_enabled) {
			slippi_ftp_cancel_uploads();
		}
		scene_monitor.uploads_enabled = 0;
		scene_monitor.upload_timer = 0;
	}
}

// Update scene monitor - should be called regularly
void slippi_scene_monitor_update(void) {
	// Don't run any scene monitoring if FTP is disabled
	if (!slippi_settings || !slippi_settings->ftp_enabled) {
		return;
	}
	
	if (!monitor_initialized) {
		slippi_scene_monitor_init();
	}
	
	// Read current scenes from memory using safe read32 function
	// Based on m-overlay: major scene is upper byte, minor scene is lower byte
	// Add basic validation to ensure we're reading valid memory
	u32 scene_data = read32(0x80479d30);
	
	// Basic sanity check - if scene data seems invalid, skip this update
	if (scene_data == 0 || scene_data == 0xFFFFFFFF) {
		return;
	}
	
	unsigned char new_major_scene = (unsigned char)(scene_data >> 24); // Major scene is in the upper byte
	unsigned char new_minor_scene = (unsigned char)(scene_data & 0xFF); // Minor scene is in the lower byte
	
	// Check for scene transitions
	if (new_major_scene != scene_monitor.current_major_scene || new_minor_scene != scene_monitor.current_minor_scene) {
		scene_monitor.previous_major_scene = scene_monitor.current_major_scene;
		scene_monitor.previous_minor_scene = scene_monitor.current_minor_scene;
		scene_monitor.current_major_scene = new_major_scene;
		scene_monitor.current_minor_scene = new_minor_scene;
		
		// Handle scene transitions
		slippi_scene_handle_transition();
	}
	
	// Handle upload timing
	if (scene_monitor.upload_timer > 0) {
		scene_monitor.upload_timer--;
		if (scene_monitor.upload_timer == 0) {
			// Timer expired, try to upload
			if (slippi_scene_is_character_select() && slippi_settings && slippi_settings->ftp_enabled) {
				slippi_ftp_upload_queued_replays();
			}
		}
	}
}

// Check if current scene is character select
int slippi_scene_is_character_select(void) {
	return (scene_monitor.current_major_scene == SCENE_VS_MODE && scene_monitor.current_minor_scene == SCENE_VS_CSS);
}

// Check if current scene is stage select
int slippi_scene_is_stage_select(void) {
	return (scene_monitor.current_major_scene == SCENE_VS_MODE && scene_monitor.current_minor_scene == SCENE_VS_SSS);
}

// Get current major scene
unsigned char slippi_scene_get_current(void) {
	return scene_monitor.current_major_scene;
}

// Cleanup scene monitor
void slippi_scene_monitor_cleanup(void) {
	if (!monitor_initialized) {
		return;
	}
	
	if (slippi_settings && slippi_settings->ftp_enabled) {
		slippi_ftp_cancel_uploads();
	}
	memset(&scene_monitor, 0, sizeof(scene_monitor));
	monitor_initialized = 0;
}
