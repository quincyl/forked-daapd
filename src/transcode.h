
#ifndef __TRANSCODE_H__
#define __TRANSCODE_H__

#ifdef HAVE_LIBEVENT2
# include <event2/buffer.h>
#else
# include <event.h>
#endif
#include "http.h"

enum transcode_profile
{
  // Transcodes the best available audio stream into PCM16 (does not add wav header)
  XCODE_PCM16_NOHEADER,
  // Transcodes the best available audio stream into PCM16 (with wav header)
  XCODE_PCM16_HEADER,
  // Transcodes the best available audio stream into MPEG3
  XCODE_MPEG3,
  // Transcodes video + audio + subtitle streams (not tested - for future use)
  XCODE_VIDEO,
};

struct transcode_ctx;

int
transcode(struct transcode_ctx *ctx, struct evbuffer *evbuf, int wanted, int *icy_timer);

int
transcode_seek(struct transcode_ctx *ctx, int ms);

struct transcode_ctx *
transcode_setup(enum transcode_profile profile, struct media_file_info *mfi, off_t *est_size);

void
transcode_cleanup(struct transcode_ctx *ctx);

int
transcode_needed(const char *user_agent, const char *client_codecs, char *file_codectype);

struct http_icy_metadata *
transcode_metadata(struct transcode_ctx *ctx, int *changed);

char *
transcode_metadata_artwork_url(struct transcode_ctx *ctx);

#endif /* !__TRANSCODE_H__ */
