/*
 * Copyright (C) 2015 Espen Jurgensen
 *
 * Adapted from ffmpeg example:
 * Copyright (c) 2010 Nicolas George
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2014 Andrey Utkin
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#include "logger.h"
#include "conffile.h"
#include "db.h"
#include "avio_evbuffer.h"
#include "transcode.h"

#define MAX_STREAMS 64

struct filter_ctx {
  AVFilterContext *buffersink_ctx;
  AVFilterContext *buffersrc_ctx;
  AVFilterGraph *filter_graph;
};

struct transcode_ctx {
  AVFormatContext *ifmt_ctx;
  AVFormatContext *ofmt_ctx;

  struct filter_ctx *filter_ctx;

  // The ffmpeg muxer writes to this buffer using the avio_evbuffer interface
  struct evbuffer *obuf;

  // Array mapping the input stream numbers that we decode to the output stream
  // numbers that we encode. So if we are decoding audio stream 3 and encoding it
  // to 0, then stream_map[3] is 0. A value of -1 means the stream is ignored.
  int stream_map[MAX_STREAMS];

  // Options for encoding and muxing
  const char *out_format;
  int transcode_video;

  // Audio
  enum AVCodecID out_audio_codec;
  int out_sample_rate;
  uint64_t out_channel_layout;
  int out_channels;
  enum AVSampleFormat out_sample_format;
  int out_byte_depth;

  // Video
  enum AVCodecID out_video_codec;
  int out_video_height;
  int out_video_width;

// TODO
  int need_resample;

  off_t offset;
  uint32_t duration;
  uint64_t samples;
  uint32_t icy_hash;

  /* WAV header */
  int wavhdr;
  uint8_t header[44];
};


static inline void
add_le16(uint8_t *dst, uint16_t val)
{
  dst[0] = val & 0xff;
  dst[1] = (val >> 8) & 0xff;
}

static inline void
add_le32(uint8_t *dst, uint32_t val)
{
  dst[0] = val & 0xff;
  dst[1] = (val >> 8) & 0xff;
  dst[2] = (val >> 16) & 0xff;
  dst[3] = (val >> 24) & 0xff;
}

static void
make_wav_header(struct transcode_ctx *ctx, off_t *est_size)
{
  uint32_t wav_len;
  int duration;

  if (ctx->duration)
    duration = ctx->duration;
  else
    duration = 3 * 60 * 1000; /* 3 minutes, in ms */

  if (ctx->samples && !ctx->need_resample)
    wav_len = ctx->out_channels * ctx->out_byte_depth * ctx->samples;
  else
    wav_len = ctx->out_channels * ctx->out_byte_depth * ctx->out_sample_rate * (duration / 1000);

  *est_size = wav_len + sizeof(ctx->header);

  memcpy(ctx->header, "RIFF", 4);
  add_le32(ctx->header + 4, 36 + wav_len);
  memcpy(ctx->header + 8, "WAVEfmt ", 8);
  add_le32(ctx->header + 16, 16);
  add_le16(ctx->header + 20, 1);
  add_le16(ctx->header + 22, ctx->out_channels);     /* channels */
  add_le32(ctx->header + 24, ctx->out_sample_rate);  /* samplerate */
  add_le32(ctx->header + 28, ctx->out_sample_rate * ctx->out_channels * ctx->out_byte_depth); /* byte rate */
  add_le16(ctx->header + 32, ctx->out_channels * ctx->out_byte_depth);                        /* block align */
  add_le16(ctx->header + 34, ctx->out_byte_depth * 8);                                        /* bits per sample */
  memcpy(ctx->header + 36, "data", 4);
  add_le32(ctx->header + 40, wav_len);
}

static int
open_profile(struct transcode_ctx *ctx, enum transcode_profile profile)
{
  switch (profile)
    {
      case XCODE_PCM16_NOHEADER:
      case XCODE_PCM16_HEADER:
        ctx->transcode_video = 0;
	ctx->out_format = "s16le";
	ctx->out_audio_codec = AV_CODEC_ID_PCM_S16LE;
	ctx->out_sample_rate = 44100;
	ctx->out_channel_layout = AV_CH_LAYOUT_STEREO;
	ctx->out_channels = 2;
	ctx->out_sample_format = AV_SAMPLE_FMT_S16;
	ctx->out_byte_depth = 2; // Bytes per sample = 16/8
	return 0;

      case XCODE_MPEG3:
        ctx->transcode_video = 0;
	return 0;

      case XCODE_VIDEO:
        ctx->transcode_video = 1;
	return 0;

      default:
	DPRINTF(E_LOG, L_XCODE, "Bug! Unknown transcoding profile\n");
	return -1;
    }
}

static int
open_input(struct transcode_ctx *ctx, const char *path)
{
  AVCodec *decoder;
  unsigned int stream_index;
  int i;
  int ret;

  ctx->ifmt_ctx = NULL;
  if ((ret = avformat_open_input(&ctx->ifmt_ctx, path, NULL, NULL)) < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Cannot open input path\n");
      return ret;
    }

// TODO close input on failure
  if ((ret = avformat_find_stream_info(ctx->ifmt_ctx, NULL)) < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Cannot find stream information\n");
      return ret;
    }

  if (ctx->ifmt_ctx->nb_streams > MAX_STREAMS)
    {
      DPRINTF(E_LOG, L_XCODE, "File '%s' has too many streams (%u)\n", path, ctx->ifmt_ctx->nb_streams);
      return -1;
    }

  for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++)
    ctx->stream_map[i] = -1;  // Default: Don't transcode

  // Find audio stream and open decoder    
  stream_index = av_find_best_stream(ctx->ifmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
  if ((stream_index < 0) || (!decoder))
    {
      DPRINTF(E_LOG, L_XCODE, "Did not find audio stream or suitable decoder for %s\n", path);
      return -1;
    }

  ret = avcodec_open2(ctx->ifmt_ctx->streams[stream_index]->codec, decoder, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Failed to open decoder for stream #%u\n", stream_index);
      return ret;
    }

  ctx->stream_map[stream_index] = 1; // Marks for transcoding - actual output stream will be set by open_output()

  // If no video then we are all done
  if (!ctx->transcode_video)
    return 0;

  // Find video stream and open decoder    
  stream_index = av_find_best_stream(ctx->ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  if ((stream_index < 0) || (!decoder))
    {
      DPRINTF(E_LOG, L_XCODE, "Did not find video stream or suitable decoder for %s\n", path);
      return -1;
    }

  ret = avcodec_open2(ctx->ifmt_ctx->streams[stream_index]->codec, decoder, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Failed to open decoder for stream #%u\n", stream_index);
      return ret;
    }

  ctx->stream_map[stream_index] = 1; // Marks for transcoding - actual output stream will be set by open_output()

  // TODO Subtitles

  return 0;
}

static int open_output(struct transcode_ctx *ctx)
{
    AVStream *out_stream;
    AVStream *in_stream;
    AVCodecContext *dec_ctx, *enc_ctx;
    AVCodec *encoder;
    int ret;
    unsigned int i;

    ctx->ofmt_ctx = NULL;
    avformat_alloc_output_context2(&ctx->ofmt_ctx, NULL, ctx->out_format, NULL);
    if (!ctx->ofmt_ctx) {
        DPRINTF(E_LOG, L_XCODE, "Could not create output context\n");
        return AVERROR_UNKNOWN;
    }

    ctx->obuf = evbuffer_new();
    if (!ctx->obuf) {
        DPRINTF(E_LOG, L_XCODE, "Could not create output evbuffer\n");
        return AVERROR_UNKNOWN;
    }
// TODO Cleanup previous allocs on failure
    ctx->ofmt_ctx->pb = avio_evbuffer_open(ctx->obuf);
    if (!ctx->ofmt_ctx->pb) {
        DPRINTF(E_LOG, L_XCODE, "Could not create output avio pb\n");
        return AVERROR_UNKNOWN;
    }

    for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++) {
        if (ctx->stream_map[i] < 0)
          continue;

        in_stream = ctx->ifmt_ctx->streams[i];
        out_stream = avformat_new_stream(ctx->ofmt_ctx, NULL);
        if (!out_stream) {
            DPRINTF(E_LOG, L_XCODE, "Failed allocating output stream\n");
            return AVERROR_UNKNOWN;
        }
	ctx->stream_map[i] = out_stream->index;
DPRINTF(E_LOG, L_XCODE, "Allocated output stream %d, %u\n", ctx->stream_map[i], out_stream->index);
        dec_ctx = in_stream->codec;
        enc_ctx = out_stream->codec;

        if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO || dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                encoder = avcodec_find_encoder(ctx->out_video_codec);
            if (!encoder) {
                DPRINTF(E_LOG, L_XCODE, "Necessary encoder from stream %u not found\n", i);
                return AVERROR_INVALIDDATA;
            }

                enc_ctx->height = ctx->out_video_height;
                enc_ctx->width = ctx->out_video_width;
                enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
                /* take first format from list of supported formats */
# ifndef HAVE_FFMPEG
  enc_ctx->pix_fmt = avcodec_find_best_pix_fmt2(encoder->pix_fmts, dec_ctx->pix_fmt, 1, NULL);
# else
  enc_ctx->pix_fmt = avcodec_find_best_pix_fmt_of_list(encoder->pix_fmts, dec_ctx->pix_fmt, 1, NULL);
# endif
                /* video time_base can be set to whatever is handy and supported by encoder */
                enc_ctx->time_base = dec_ctx->time_base;
            } else {
                encoder = avcodec_find_encoder(ctx->out_audio_codec);
            if (!encoder) {
                DPRINTF(E_LOG, L_XCODE, "Necessary encoder from stream %u not found\n", i);
                return AVERROR_INVALIDDATA;
            }
                enc_ctx->sample_rate = ctx->out_sample_rate;
                enc_ctx->channel_layout = ctx->out_channel_layout;
                enc_ctx->channels = ctx->out_channels;
                enc_ctx->sample_fmt = ctx->out_sample_format;
                enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
            }

            ret = avcodec_open2(enc_ctx, encoder, NULL);
            if (ret < 0) {
                DPRINTF(E_LOG, L_XCODE, "Cannot open video encoder for stream #%u\n", i);
                return ret;
            }
        } else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN) {
            DPRINTF(E_LOG, L_XCODE, "Elementary stream #%d is of unknown type, cannot proceed\n", i);
            return AVERROR_INVALIDDATA;
        } else {
            /* if this stream must be remuxed */
            DPRINTF(E_LOG, L_XCODE, "Stream will be remuxed\n");
            ret = avcodec_copy_context(enc_ctx, dec_ctx);
            if (ret < 0) {
                DPRINTF(E_LOG, L_XCODE, "Copying stream context failed\n");
                return ret;
            }
        }

        if (ctx->ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            enc_ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }

    /* init muxer, write output header */
    ret = avformat_write_header(ctx->ofmt_ctx, NULL);
    if (ret < 0) {
        DPRINTF(E_LOG, L_XCODE, "Error occurred when writing header to output buffer\n");
        return ret;
    }

    return 0;
}

static int
init_filter(struct filter_ctx *filter_ctx, AVCodecContext *dec_ctx, AVCodecContext *enc_ctx, const char *filter_spec)
{
    char args[512];
    int ret = 0;
    AVFilter *buffersrc = NULL;
    AVFilter *buffersink = NULL;
    AVFilterContext *buffersrc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterGraph *filter_graph = avfilter_graph_alloc();

    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        buffersrc = avfilter_get_by_name("buffer");
        buffersink = avfilter_get_by_name("buffersink");
        if (!buffersrc || !buffersink) {
            DPRINTF(E_LOG, L_XCODE, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        snprintf(args, sizeof(args),
                "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                dec_ctx->time_base.num, dec_ctx->time_base.den,
                dec_ctx->sample_aspect_ratio.num,
                dec_ctx->sample_aspect_ratio.den);

        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Cannot create buffer source\n");
            goto end;
        }

        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Cannot create buffer sink\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "pix_fmts", (uint8_t*)&enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt), AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Cannot set output pixel format\n");
            goto end;
        }
    } else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        buffersrc = avfilter_get_by_name("abuffer");
        buffersink = avfilter_get_by_name("abuffersink");
        if (!buffersrc || !buffersink) {
            DPRINTF(E_LOG, L_XCODE, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        if (!dec_ctx->channel_layout)
            dec_ctx->channel_layout =
                av_get_default_channel_layout(dec_ctx->channels);
        snprintf(args, sizeof(args),
                "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
                dec_ctx->time_base.num, dec_ctx->time_base.den, dec_ctx->sample_rate,
                av_get_sample_fmt_name(dec_ctx->sample_fmt),
                dec_ctx->channel_layout);
        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                args, NULL, filter_graph);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Cannot create audio buffer source\n");
            goto end;
        }

        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Cannot create audio buffer sink\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "sample_fmts", (uint8_t*)&enc_ctx->sample_fmt, sizeof(enc_ctx->sample_fmt), AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Cannot set output sample format\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "channel_layouts",
                (uint8_t*)&enc_ctx->channel_layout,
                sizeof(enc_ctx->channel_layout), AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Cannot set output channel layout\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "sample_rates",
                (uint8_t*)&enc_ctx->sample_rate, sizeof(enc_ctx->sample_rate),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Cannot set output sample rate\n");
            goto end;
        }
    } else {
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    /* Endpoints for the filter graph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_spec,
                    &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

    /* Fill filtering context */
    filter_ctx->buffersrc_ctx = buffersrc_ctx;
    filter_ctx->buffersink_ctx = buffersink_ctx;
    filter_ctx->filter_graph = filter_graph;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

static int
init_filters(struct transcode_ctx *ctx)
{
  AVCodecContext *dec_ctx, *enc_ctx;
  const char *filter_spec;
  unsigned int i;
  int stream_nb;
  int ret;

  ctx->filter_ctx = av_malloc_array(ctx->ifmt_ctx->nb_streams, sizeof(*ctx->filter_ctx));
  if (!ctx->filter_ctx)
    return AVERROR(ENOMEM);

  for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++)
    {
      ctx->filter_ctx[i].buffersrc_ctx  = NULL;
      ctx->filter_ctx[i].buffersink_ctx = NULL;
      ctx->filter_ctx[i].filter_graph   = NULL;

      stream_nb = ctx->stream_map[i];
      if (stream_nb < 0)
	continue;

      dec_ctx = ctx->ifmt_ctx->streams[i]->codec;
      enc_ctx = ctx->ofmt_ctx->streams[stream_nb]->codec;

      if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
	filter_spec = "null"; /* passthrough (dummy) filter for video */
      else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
	filter_spec = "anull"; /* passthrough (dummy) filter for audio */
      else
	continue;

      ret = init_filter(&ctx->filter_ctx[i], dec_ctx, enc_ctx, filter_spec);
      if (ret)
	return ret;
    }

  return 0;
}

static int
encode_write_frame(struct transcode_ctx *ctx, AVFrame *filt_frame, unsigned int stream_index, int *got_frame)
{
  AVStream *out_stream;
  AVPacket enc_pkt;
  int ret;
  int got_frame_local;
  int stream_nb;
  int (*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *) =
        (ctx->ifmt_ctx->streams[stream_index]->codec->codec_type ==
         AVMEDIA_TYPE_VIDEO) ? avcodec_encode_video2 : avcodec_encode_audio2;

  if (!got_frame)
    got_frame = &got_frame_local;

  stream_nb = ctx->stream_map[stream_index];
  out_stream = ctx->ofmt_ctx->streams[stream_nb];

  /* encode filtered frame */
  enc_pkt.data = NULL;
  enc_pkt.size = 0;
  av_init_packet(&enc_pkt);
  ret = enc_func(out_stream->codec, &enc_pkt, filt_frame, got_frame);
  av_frame_free(&filt_frame);
  if (ret < 0)
    return ret;
  if (!(*got_frame))
    return 0;

  /* prepare packet for muxing */
  enc_pkt.stream_index = stream_nb;
  av_packet_rescale_ts(&enc_pkt, out_stream->codec->time_base, out_stream->time_base);

  /* mux encoded frame */
  ret = av_interleaved_write_frame(ctx->ofmt_ctx, &enc_pkt);
  return ret;
}

static int filter_encode_write_frame(struct transcode_ctx *ctx, AVFrame *frame, unsigned int stream_index)
{
    int ret;
    AVFrame *filt_frame;

    /* push the decoded frame into the filtergraph */
    ret = av_buffersrc_add_frame_flags(ctx->filter_ctx[stream_index].buffersrc_ctx, frame, 0);
    if (ret < 0) {
        DPRINTF(E_LOG, L_XCODE, "Error while feeding the filtergraph\n");
        return ret;
    }

    /* pull filtered frames from the filtergraph */
    while (1) {
        filt_frame = av_frame_alloc();
        if (!filt_frame) {
            ret = AVERROR(ENOMEM);
            break;
        }
        ret = av_buffersink_get_frame(ctx->filter_ctx[stream_index].buffersink_ctx, filt_frame);
        if (ret < 0) {
            /* if no more frames for output - returns AVERROR(EAGAIN)
             * if flushed and no more frames for output - returns AVERROR_EOF
             * rewrite retcode to 0 to show it as normal procedure completion
             */
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                ret = 0;
            av_frame_free(&filt_frame);
            break;
        }

        filt_frame->pict_type = AV_PICTURE_TYPE_NONE;
        ret = encode_write_frame(ctx, filt_frame, stream_index, NULL);
        if (ret < 0)
            break;
    }

    return ret;
}

static int flush_encoder(struct transcode_ctx *ctx, unsigned int stream_index)
{
    int ret;
    int got_frame;

    if (!(ctx->ofmt_ctx->streams[stream_index]->codec->codec->capabilities &
                CODEC_CAP_DELAY))
        return 0;

    while (1) {
        DPRINTF(E_LOG, L_XCODE, "Flushing stream #%u encoder\n", stream_index);
        ret = encode_write_frame(ctx, NULL, stream_index, &got_frame);
        if (ret < 0)
            break;
        if (!got_frame)
            return 0;
    }
    return ret;
}

int
transcode_setup(struct transcode_ctx **nctx, enum transcode_profile profile, struct media_file_info *mfi, off_t *est_size)
{
    struct transcode_ctx *ctx;
    int ret;

    ctx = malloc(sizeof(struct transcode_ctx));
    if (!ctx)
	goto end;
    memset(ctx, 0, sizeof(struct transcode_ctx));

    if ((ret = open_profile(ctx, profile)) < 0)
        goto end;
    if ((ret = open_input(ctx, mfi->path)) < 0)
        goto end;
    if ((ret = open_output(ctx)) < 0)
        goto end;
    if ((ret = init_filters(ctx)) < 0)
        goto end;

  ctx->duration = mfi->song_length;
  ctx->samples = mfi->sample_count;

  if (profile == XCODE_PCM16_HEADER)
    {
      ctx->wavhdr = 1;
      make_wav_header(ctx, est_size);
    }

    *nctx = ctx;

    return 0;

end:
    transcode_cleanup(ctx);
    return -1;
}

int
transcode(struct transcode_ctx *ctx, struct evbuffer *evbuf, int wanted, int *icy_timer)
{
  AVPacket packet = { .data = NULL, .size = 0 };
  AVStream *in_stream, *out_stream;
  AVFrame *frame = NULL;
  int stream_nb;
  int processed;
  int stop;
  int got_frame;
  int (*dec_func)(AVCodecContext *, AVFrame *, int *, const AVPacket *);
  int ret;

  if (ctx->wavhdr && (ctx->offset == 0))
    {
      evbuffer_add(evbuf, ctx->header, sizeof(ctx->header));
      processed += sizeof(ctx->header);
      ctx->offset += sizeof(ctx->header);
    }

  ret = 0;
  processed = 0;
  stop = 0;
  while ((processed < wanted) && !stop)
    {
      if ((ret = av_read_frame(ctx->ifmt_ctx, &packet)) < 0)
	break;

      stream_nb = ctx->stream_map[packet.stream_index];
      DPRINTF(E_LOG, L_XCODE, "Demuxer gave frame of stream_index %u, stream_nb %d\n", packet.stream_index, stream_nb);
      if (stream_nb < 0)
	{
	  av_free_packet(&packet);
	  continue;
	}

      in_stream = ctx->ifmt_ctx->streams[packet.stream_index];
      out_stream = ctx->ofmt_ctx->streams[stream_nb];

      if (ctx->filter_ctx[packet.stream_index].filter_graph)
	{
	  DPRINTF(E_LOG, L_XCODE, "Going to reencode&filter the frame\n");
	  frame = av_frame_alloc();
	  if (!frame)
	    {
	      ret = AVERROR(ENOMEM);
	      break;
	    }

	  av_packet_rescale_ts(&packet, in_stream->time_base, in_stream->codec->time_base);

	  dec_func = (in_stream->codec->codec_type == AVMEDIA_TYPE_VIDEO) ? avcodec_decode_video2 : avcodec_decode_audio4;
	  ret = dec_func(in_stream->codec, frame, &got_frame, &packet);
	  if (ret < 0)
	    {
	      av_frame_free(&frame);
	      DPRINTF(E_LOG, L_XCODE, "Decoding failed\n");
	      break;
	    }

	  if (got_frame)
	    {
	      frame->pts = av_frame_get_best_effort_timestamp(frame);
	      ret = filter_encode_write_frame(ctx, frame, packet.stream_index);
	      av_frame_free(&frame);
	      if (ret < 0)
		goto end;
	    }
	  else
	    av_frame_free(&frame);
	}
      else
	{
	  DPRINTF(E_LOG, L_XCODE, "Remux without reencoding\n");
	  /* remux this frame without reencoding */
	  av_packet_rescale_ts(&packet, in_stream->time_base, out_stream->time_base);

	  ret = av_interleaved_write_frame(ctx->ofmt_ctx, &packet);
	  if (ret < 0)
	    goto end;
	}

      av_free_packet(&packet);

      processed += evbuffer_get_length(ctx->obuf);
      DPRINTF(E_LOG, L_XCODE, "PROCESSED is %d\n", processed);

      evbuffer_add_buffer(evbuf, ctx->obuf);
    }

  av_free_packet(&packet);
  av_frame_free(&frame);

end:
  if (ret < 0)
    DPRINTF(E_LOG, L_XCODE, "Error occurred: %s\n", av_err2str(ret));
  else
    ret = processed;

  return ret;
}

void
transcode_cleanup(struct transcode_ctx *ctx)
{
  int i;
  int ret;

  /* flush filters and encoders */
  for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++) {
        /* flush filter */
        if (!ctx->filter_ctx[i].filter_graph)
            continue;
        ret = filter_encode_write_frame(ctx, NULL, i);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Flushing filter failed\n");
        }

        /* flush encoder */
        ret = flush_encoder(ctx, i);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Flushing encoder failed\n");
        }
    }

    av_write_trailer(ctx->ofmt_ctx);

    for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++)
      {
	
        avcodec_close(ctx->ifmt_ctx->streams[i]->codec);
        if (ctx->ofmt_ctx && ctx->ofmt_ctx->nb_streams > i && ctx->ofmt_ctx->streams[i] && ctx->ofmt_ctx->streams[i]->codec)
            avcodec_close(ctx->ofmt_ctx->streams[i]->codec);
        if (ctx->filter_ctx && ctx->filter_ctx[i].filter_graph)
            avfilter_graph_free(&ctx->filter_ctx[i].filter_graph);
      }
    av_free(ctx->filter_ctx);
    avformat_close_input(&ctx->ifmt_ctx);
    avio_evbuffer_close(ctx->ofmt_ctx->pb);
    evbuffer_free(ctx->obuf);
    avformat_free_context(ctx->ofmt_ctx);
}

int
transcode_seek(struct transcode_ctx *ctx, int ms)
{
return -1;
}

int
transcode_needed(const char *user_agent, const char *client_codecs, char *file_codectype)
{
return 0;
}

int
transcode_wav2mpeg(uint8_t *wavbuf, int wavbufsize, uint8_t *mpegbuf, int mpegbufsize)
{
return -1;
}

void
transcode_metadata(struct transcode_ctx *ctx, struct http_icy_metadata **metadata, int *changed)
{
}

void
transcode_metadata_artwork_url(struct transcode_ctx *ctx, char **artwork_url)
{
}

