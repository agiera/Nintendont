#ifndef __SLIPPI_COMPRESS_H__
#define __SLIPPI_COMPRESS_H__

#ifdef SLPZ_HOST_TEST
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
#else
#include "global.h"
#include "string.h"
#endif

// Receives encoder output; returns < 0 to abort.
typedef int (*slpz_sink_t)(const u8* data, u32 len);

// Starts a new .slpz stream (raw DEFLATE of the delta-filtered .slp bytes).
void slpz_reset(void);
// Encodes len bytes and sync-flushes, so everything given so far is decodable.
int slpz_write(const u8* data, u32 len, slpz_sink_t sink);
// Emits the final empty block that ends the DEFLATE stream.
int slpz_finish(slpz_sink_t sink);

#endif
