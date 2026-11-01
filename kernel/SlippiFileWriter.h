#ifndef __SLIPPI_FILE_WRITER_H__
#define __SLIPPI_FILE_WRITER_H__

#include "global.h"

void SlippiFileWriterInit(bool led);
void SlippiFileWriterUpdateRegisters();
void SlippiFileWriterShutdown();

// Shared controller-metadata parse used by both the local .slp footer writer and
// the streamed FTP .meta.json sidecar. Reads SI channel `ch` (0..3) from the
// CMD 0xA0xx shared-memory region, verifies the call/tag guards, reassembles the
// chunked payload into `dest`, and validates it as a flat UBJSON string dict.
// Returns the validated byte count (including the enclosing { and }), or 0 when
// the channel has no valid controller metadata.
u16 SlippiReadControllerMetadata(int ch, u8 *dest, u16 maxLen);

// Raw PPC probe bookkeeping for SI channel `ch`, for .diag.json troubleshooting.
typedef struct {
	u32 tag;
	u32 calls;
	u32 n_chunks;
	u32 comcsr;
	u32 iobuf0;
	s32 cmd_idx;
	u32 attempts;
	u32 rearms;
	u32 runs;
	u16 valid_len;
} SlippiControllerProbeState;

void SlippiGetControllerProbeState(int ch, SlippiControllerProbeState *out);

#endif
