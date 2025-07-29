/*
Slippi Scene Monitor for Nintendont
Monitors game scenes to trigger FTP uploads at appropriate times
*/

#ifndef _SLIPPI_SCENE_MONITOR_H_
#define _SLIPPI_SCENE_MONITOR_H_

// Scene constants based on m-overlay melee.lua
#define SCENE_VS_MODE			0x02
#define SCENE_VS_CSS			0x00  // Character Select Screen
#define SCENE_VS_SSS			0x01  // Stage Select Screen  
#define SCENE_VS_INGAME			0x02  // In Game
#define SCENE_VS_POSTGAME		0x04  // Post Game

// Scene monitor state
typedef struct {
	unsigned char current_major_scene;
	unsigned char current_minor_scene;
	unsigned char previous_major_scene;
	unsigned char previous_minor_scene;
	int uploads_enabled;
	int upload_timer;
} slippi_scene_monitor_t;

// Function prototypes
int slippi_scene_monitor_init(void);
void slippi_scene_monitor_update(void);
void slippi_scene_monitor_cleanup(void);
int slippi_scene_is_character_select(void);
int slippi_scene_is_stage_select(void);
unsigned char slippi_scene_get_current(void);
static void slippi_scene_handle_transition(void);

#endif /* _SLIPPI_SCENE_MONITOR_H_ */
