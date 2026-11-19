// .slpz encoder: per-player XOR delta of frame events, then greedy LZ77 with
// fixed-Huffman DEFLATE blocks. Must match backend/app/services/slpz.py in
// slippi-replay-hosting-server.
#include "SlippiCompress.h"

#define WSIZE 4096
#define HBITS 12
#define HSIZE (1 << HBITS)
#define CHAIN 16
#define MIN_MATCH 3
#define MAX_MATCH 258
#define OUT_SIZE 2048

#define HEADER_LEN 15
#define DELTA_MAX_PAYLOAD 128
#define DELTA_FIRST_BYTE 6
#define CMD_EVENT_PAYLOADS 0x35
#define CMD_PRE_FRAME 0x37
#define CMD_POST_FRAME 0x38
#define NO_SLOT 0xFF

enum { D_HEADER, D_EXPECT_CMD, D_SIZES_LEN, D_SIZES, D_IN_EVENT, D_PASS };

static struct {
	u8 state;
	u8 cmd;
	u8 player;
	u8 slot;
	u8 triplet[3];
	u8 triplet_len;
	u8 sizes_known;
	u16 pos;
	u16 index;
	u16 remaining;
	u16 have;
	u16 sizes[256];
	u8 frame[2][4];
	u8 prev[16][DELTA_MAX_PAYLOAD];
} delta;

// Window positions are stored +1 so 0 means "none".
static u8 window[2 * WSIZE];
static u16 head[HSIZE];
static u16 chain[WSIZE];
static u32 strstart;

static u8 out[OUT_SIZE];
static u32 out_len;
static u32 bitbuf;
static u32 bitcnt;
static int sink_error;

static const u16 len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const u8 len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const u16 dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const u8 dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void flush_out(slpz_sink_t sink)
{
	if (out_len && !sink_error && sink(out, out_len) < 0)
		sink_error = 1;
	out_len = 0;
}

static void put_bits(u32 value, u32 nbits, slpz_sink_t sink)
{
	bitbuf |= value << bitcnt;
	bitcnt += nbits;
	while (bitcnt >= 8) {
		out[out_len++] = (u8)bitbuf;
		bitbuf >>= 8;
		bitcnt -= 8;
		if (out_len == OUT_SIZE)
			flush_out(sink);
	}
}

// Huffman codes are packed most-significant bit first.
static void put_code(u32 code, u32 nbits, slpz_sink_t sink)
{
	u32 rev = 0, i;
	for (i = 0; i < nbits; i++) {
		rev = (rev << 1) | (code & 1);
		code >>= 1;
	}
	put_bits(rev, nbits, sink);
}

static void put_symbol(u32 sym, slpz_sink_t sink)
{
	if (sym < 144)
		put_code(0x30 + sym, 8, sink);
	else if (sym < 256)
		put_code(0x190 + sym - 144, 9, sink);
	else if (sym < 280)
		put_code(sym - 256, 7, sink);
	else
		put_code(0xC0 + sym - 280, 8, sink);
}

static void put_match(u32 length, u32 dist, slpz_sink_t sink)
{
	u32 i = 28, j = 29;
	while (len_base[i] > length)
		i--;
	put_symbol(257 + i, sink);
	if (len_extra[i])
		put_bits(length - len_base[i], len_extra[i], sink);
	while (dist_base[j] > dist)
		j--;
	put_code(j, 5, sink);
	if (dist_extra[j])
		put_bits(dist - dist_base[j], dist_extra[j], sink);
}

static u32 hash_at(u32 p)
{
	u32 h = ((u32)window[p] << 16) | ((u32)window[p + 1] << 8) | window[p + 2];
	return (h * 2654435761u) >> (32 - HBITS);
}

static void insert(u32 p)
{
	u32 h = hash_at(p);
	chain[p & (WSIZE - 1)] = head[h];
	head[h] = (u16)(p + 1);
}

static void slide(void)
{
	u32 i;
	memcpy(window, window + WSIZE, WSIZE);
	for (i = 0; i < HSIZE; i++)
		head[i] = head[i] > WSIZE ? head[i] - WSIZE : 0;
	for (i = 0; i < WSIZE; i++)
		chain[i] = chain[i] > WSIZE ? chain[i] - WSIZE : 0;
	strstart -= WSIZE;
}

static u8 delta_start_event(u8 byte)
{
	u16 size;
	if (!delta.sizes_known) {
		delta.state = byte == CMD_EVENT_PAYLOADS ? D_SIZES_LEN : D_PASS;
		return byte;
	}
	size = delta.sizes[byte];
	if (!size) {
		delta.state = D_PASS;
		return byte;
	}
	delta.cmd = byte;
	delta.remaining = size;
	delta.index = 0;
	delta.slot = NO_SLOT;
	delta.state = D_IN_EVENT;
	return byte;
}

static u8 delta_event_byte(u8 byte)
{
	u16 index = delta.index;
	u8 slot = delta.slot;
	if (delta.cmd == CMD_PRE_FRAME || delta.cmd == CMD_POST_FRAME) {
		if (index < 4) {
			u8 original = byte;
			byte ^= delta.frame[delta.cmd - CMD_PRE_FRAME][index];
			delta.frame[delta.cmd - CMD_PRE_FRAME][index] = original;
		} else if (index == 4) {
			delta.player = byte;
		} else if (index == 5) {
			if (delta.player < 4 && byte < 2 && delta.sizes[delta.cmd] <= DELTA_MAX_PAYLOAD)
				delta.slot = (u8)((delta.cmd - CMD_PRE_FRAME) * 8 + delta.player * 2 + byte);
		} else if (index >= DELTA_FIRST_BYTE && slot != NO_SLOT) {
			u8 original = byte;
			if (delta.have & (1u << slot))
				byte ^= delta.prev[slot][index];
			delta.prev[slot][index] = original;
		}
	}
	delta.index++;
	if (--delta.remaining == 0) {
		if (delta.slot != NO_SLOT)
			delta.have |= (u16)(1u << delta.slot);
		delta.state = D_EXPECT_CMD;
	}
	return byte;
}

static u8 delta_byte(u8 byte)
{
	switch (delta.state) {
	case D_HEADER:
		if (++delta.pos == HEADER_LEN)
			delta.state = D_EXPECT_CMD;
		return byte;
	case D_EXPECT_CMD:
		return delta_start_event(byte);
	case D_SIZES_LEN:
		delta.sizes[CMD_EVENT_PAYLOADS] = byte;
		delta.remaining = byte ? byte - 1 : 0;
		delta.triplet_len = 0;
		delta.sizes_known = 1;
		delta.state = delta.remaining ? D_SIZES : D_EXPECT_CMD;
		return byte;
	case D_SIZES:
		delta.triplet[delta.triplet_len++] = byte;
		if (delta.triplet_len == 3) {
			delta.sizes[delta.triplet[0]] = (u16)((delta.triplet[1] << 8) | delta.triplet[2]);
			delta.triplet_len = 0;
		}
		if (--delta.remaining == 0)
			delta.state = D_EXPECT_CMD;
		return byte;
	case D_IN_EVENT:
		return delta_event_byte(byte);
	default:
		return byte;
	}
}

static void compress_range(u32 start, u32 end, slpz_sink_t sink)
{
	u32 p = start;
	while (p < end) {
		u32 best = 0, best_pos = 0;
		if (p + MIN_MATCH <= end) {
			u32 limit = end - p > MAX_MATCH ? MAX_MATCH : end - p;
			u32 cand = head[hash_at(p)];
			u32 tries = CHAIN;
			// chain[] holds one window of history, so older entries are stale.
			while (cand && p - (cand - 1) < WSIZE && tries--) {
				const u8* a = window + cand - 1;
				const u8* b = window + p;
				u32 n = 0;
				while (n < limit && a[n] == b[n])
					n++;
				if (n > best) {
					best = n;
					best_pos = cand - 1;
					if (n == limit)
						break;
				}
				cand = chain[(cand - 1) & (WSIZE - 1)];
			}
			insert(p);
		}
		if (best >= MIN_MATCH) {
			u32 q, stop = p + best;
			put_match(best, p - best_pos, sink);
			for (q = p + 1; q < stop && q + MIN_MATCH <= end; q++)
				insert(q);
			p = stop;
		} else {
			put_symbol(window[p], sink);
			p++;
		}
	}
}

void slpz_reset(void)
{
	memset(&delta, 0, sizeof(delta));
	memset(head, 0, sizeof(head));
	memset(chain, 0, sizeof(chain));
	strstart = 0;
	out_len = 0;
	bitbuf = 0;
	bitcnt = 0;
	sink_error = 0;
}

int slpz_write(const u8* data, u32 len, slpz_sink_t sink)
{
	put_bits(1 << 1, 3, sink);  // BFINAL=0, BTYPE=01 (fixed Huffman)
	while (len) {
		u32 n, i;
		if (strstart == 2 * WSIZE)
			slide();
		n = 2 * WSIZE - strstart;
		if (n > len)
			n = len;
		for (i = 0; i < n; i++)
			window[strstart + i] = delta_byte(data[i]);
		compress_range(strstart, strstart + n, sink);
		strstart += n;
		data += n;
		len -= n;
	}
	put_symbol(256, sink);
	// Sync flush: an empty stored block ends on a byte boundary.
	put_bits(0, 3, sink);
	if (bitcnt)
		put_bits(0, 8 - bitcnt, sink);
	put_bits(0x0000, 16, sink);
	put_bits(0xFFFF, 16, sink);
	flush_out(sink);
	return sink_error ? -1 : 0;
}

int slpz_finish(slpz_sink_t sink)
{
	put_bits(1 | (1 << 1), 3, sink);  // BFINAL=1, BTYPE=01
	put_symbol(256, sink);
	if (bitcnt)
		put_bits(0, 8 - bitcnt, sink);
	flush_out(sink);
	return sink_error ? -1 : 0;
}
