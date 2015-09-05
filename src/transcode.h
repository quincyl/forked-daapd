
#ifndef __TRANSCODE_H__
#define __TRANSCODE_H__

#ifdef HAVE_LIBEVENT2
# include <event2/buffer.h>
#else
# include <event.h>
#endif
#include "http.h"

#define XCODE_WAVHEADER (1 << 14)
#define XCODE_HAS_VIDEO (1 << 15)

enum transcode_profile
{
  // Transcodes the best available audio stream into PCM16 (does not add wav header)
  XCODE_PCM16_NOHEADER = 1,
  // Transcodes the best available audio stream into PCM16 (with wav header)
  XCODE_PCM16_HEADER   = XCODE_WAVHEADER | 2,
  // Transcodes the best available audio stream into MP3
  XCODE_MP3            = 3,
  // Transcodes video + audio + subtitle streams (not tested - for future use)
  XCODE_H264_AAC       = XCODE_HAS_VIDEO | 4,
};

struct decode_ctx;
struct encode_ctx;
struct transcode_ctx;
struct decoded_packet;

// Setting up
struct decode_ctx *
transcode_decode_setup(struct media_file_info *mfi, int decode_video);

struct encode_ctx *
transcode_encode_setup(struct decode_ctx *src_ctx, enum transcode_profile profile, off_t *est_size);

struct transcode_ctx *
transcode_setup(struct media_file_info *mfi, enum transcode_profile profile, off_t *est_size);

int
transcode_needed(const char *user_agent, const char *client_codecs, char *file_codectype);

// Cleaning up
void
transcode_decode_cleanup(struct decode_ctx *ctx);

void
transcode_encode_cleanup(struct encode_ctx *ctx, struct decode_ctx *src_ctx);

void
transcode_cleanup(struct transcode_ctx *ctx);

void
transcode_decoded_free(struct decoded_packet *decoded);

// Transcoding
struct decoded_packet *
transcode_decode(struct decode_ctx *ctx);

int
transcode_encode(struct evbuffer *evbuf, struct decoded_packet *decoded, struct encode_ctx *ctx);

int
transcode(struct transcode_ctx *ctx, struct evbuffer *evbuf, int wanted, int *icy_timer);

// Seeking
int
transcode_seek(struct transcode_ctx *ctx, int ms);

// Metadata
struct http_icy_metadata *
transcode_metadata(struct transcode_ctx *ctx, int *changed);

char *
transcode_metadata_artwork_url(struct transcode_ctx *ctx);

#endif /* !__TRANSCODE_H__ */
