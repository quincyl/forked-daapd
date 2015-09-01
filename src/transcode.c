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
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#ifdef HAVE_LIBAVFILTER
# include <libavfilter/avcodec.h>
#else
# include "ffmpeg-compat.c"
#endif

#include "logger.h"
#include "conffile.h"
#include "db.h"
#include "avio_evbuffer.h"
#include "transcode.h"

// Interval between ICY metadata checks for streams, in seconds
#define METADATA_ICY_INTERVAL 5
// Maximum number of streams in a file that we will accept
#define MAX_STREAMS 64

static char *default_codecs = "mpeg,wav";
static char *roku_codecs = "mpeg,mp4a,wma,wav";
static char *itunes_codecs = "mpeg,mp4a,mp4v,alac,wav";

struct filter_ctx {
  AVFilterContext *buffersink_ctx;
  AVFilterContext *buffersrc_ctx;
  AVFilterGraph *filter_graph;
};

struct transcode_ctx {
  // Input and output format context
  AVFormatContext *ifmt_ctx;
  AVFormatContext *ofmt_ctx;

  // Will point to the max 3 streams that we will transcode
  AVStream *audio_stream;
  AVStream *video_stream;
  AVStream *subtitle_stream;

  // We use filters to resample
  struct filter_ctx *filter_ctx;

  // The ffmpeg muxer writes to this buffer using the avio_evbuffer interface
  struct evbuffer *obuf;

  // Array mapping the input stream numbers that we decode to the output stream
  // numbers that we encode. So if we are decoding audio stream 3 and encoding it
  // to 0, then stream_map[3] is 0. A value of -1 means the stream is ignored.
  int stream_map[MAX_STREAMS];

  // Used for seeking
  int resume;
  AVPacket seek_packet;
  int64_t prev_pts[MAX_STREAMS];
  int64_t offset_pts[MAX_STREAMS];

  // Settings for encoding and muxing
  const char *out_format;
  int transcode_video;

  // Audio settings
  enum AVCodecID out_audio_codec;
  int out_sample_rate;
  uint64_t out_channel_layout;
  int out_channels;
  enum AVSampleFormat out_sample_format;
  int out_byte_depth;

  // Video settings
  enum AVCodecID out_video_codec;
  int out_video_height;
  int out_video_width;

  // How many output bytes we have processed in total
  off_t offset;

  // Duration and total sample count
  uint32_t duration;
  uint64_t samples;

  // Used to check for ICY metadata changes at certain intervals
  uint32_t icy_interval;
  uint32_t icy_hash;

  // WAV header
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
  int need_resample;

  if (ctx->duration)
    duration = ctx->duration;
  else
    duration = 3 * 60 * 1000; /* 3 minutes, in ms */

  need_resample = (ctx->audio_stream->codec->sample_fmt != ctx->out_sample_format)
                   || (ctx->audio_stream->codec->channels != ctx->out_channels)
                   || (ctx->audio_stream->codec->sample_rate != ctx->out_sample_rate);

  if (ctx->samples && !need_resample)
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
init_profile(struct transcode_ctx *ctx, enum transcode_profile profile)
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

      case XCODE_MP3:
        ctx->transcode_video = 0;
	ctx->out_format = "mp3";
	ctx->out_audio_codec = AV_CODEC_ID_MP3;
	ctx->out_sample_rate = 44100;
	ctx->out_channel_layout = AV_CH_LAYOUT_STEREO;
	ctx->out_channels = 2;
	ctx->out_sample_format = AV_SAMPLE_FMT_S16P;
	ctx->out_byte_depth = 2; // Bytes per sample = 16/8
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
open_input(struct transcode_ctx *ctx, struct media_file_info *mfi)
{
  AVDictionary *options;
  AVCodec *decoder;
  unsigned int stream_index;
  int i;
  int ret;

  options = NULL;
  ctx->ifmt_ctx = NULL;

# ifndef HAVE_FFMPEG
  // Without this, libav is slow to probe some internet streams, which leads to RAOP timeouts
  if (mfi->data_kind == DATA_KIND_URL)
    {
      ctx->ifmt_ctx = avformat_alloc_context();
      ctx->ifmt_ctx->probesize = 64000;
    }
# endif

  if (mfi->data_kind == DATA_KIND_URL)
    av_dict_set(&options, "icy", "1", 0);

  ret = avformat_open_input(&ctx->ifmt_ctx, mfi->path, NULL, &options);
  if (options)
    av_dict_free(&options);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Cannot open input path\n");
      return -1;
    }

  ret = avformat_find_stream_info(ctx->ifmt_ctx, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Cannot find stream information\n");
      goto out_fail;
    }

  if (ctx->ifmt_ctx->nb_streams > MAX_STREAMS)
    {
      DPRINTF(E_LOG, L_XCODE, "File '%s' has too many streams (%u)\n", mfi->path, ctx->ifmt_ctx->nb_streams);
      goto out_fail;
    }

  for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++)
    ctx->stream_map[i] = -1;  // Default: Don't transcode

  // Find audio stream and open decoder    
  stream_index = av_find_best_stream(ctx->ifmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
  if ((stream_index < 0) || (!decoder))
    {
      DPRINTF(E_LOG, L_XCODE, "Did not find audio stream or suitable decoder for %s\n", mfi->path);
      goto out_fail;
    }

  ctx->ifmt_ctx->streams[stream_index]->codec->request_sample_fmt = ctx->out_sample_format;
  ctx->ifmt_ctx->streams[stream_index]->codec->request_channel_layout = ctx->out_channel_layout;

// Disabled to see if it is still required
//  if (decoder->capabilities & CODEC_CAP_TRUNCATED)
//    ctx->ifmt_ctx->streams[stream_index]->codec->flags |= CODEC_FLAG_TRUNCATED;

  ret = avcodec_open2(ctx->ifmt_ctx->streams[stream_index]->codec, decoder, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Failed to open decoder for stream #%u\n", stream_index);
      goto out_fail;
    }

  // Mark for transcoding - actual output stream will be set by open_output()
  ctx->stream_map[stream_index] = 1;
  ctx->audio_stream = ctx->ifmt_ctx->streams[stream_index];

  // If no video then we are all done
  if (!ctx->transcode_video)
    return 0;

  // Find video stream and open decoder    
  stream_index = av_find_best_stream(ctx->ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  if ((stream_index < 0) || (!decoder))
    {
      DPRINTF(E_LOG, L_XCODE, "Did not find video stream or suitable decoder for %s\n", mfi->path);

      // Continue without video
      ctx->transcode_video = 0;
      return 0;
    }

  ret = avcodec_open2(ctx->ifmt_ctx->streams[stream_index]->codec, decoder, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Failed to open decoder for stream #%u\n", stream_index);

      ctx->transcode_video = 0;
      return 0;
    }

  // Mark for transcoding - actual output stream will be set by open_output()
  ctx->stream_map[stream_index] = 1;
  ctx->video_stream = ctx->ifmt_ctx->streams[stream_index];

  // Find a (random) subtitle stream which will be remuxed
  stream_index = av_find_best_stream(ctx->ifmt_ctx, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
  if (stream_index >= 0)
    {
      ctx->stream_map[stream_index] = 1;
      ctx->subtitle_stream = ctx->ifmt_ctx->streams[stream_index];
    }

  return 0;

 out_fail:
  avformat_close_input(&ctx->ifmt_ctx);

  return -1;
}

static void
close_input(struct transcode_ctx *ctx)
{
  AVCodecContext *dec_ctx;
  int i;

  for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++)
    {
      if (ctx->stream_map[i] < 0)
	continue;

      dec_ctx = ctx->ifmt_ctx->streams[i]->codec;
      if ((dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) || (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO))
	avcodec_close(dec_ctx);
    }

  avformat_close_input(&ctx->ifmt_ctx);
}

static int
open_output(struct transcode_ctx *ctx)
{
  AVStream *out_stream;
  AVStream *in_stream;
  AVCodecContext *dec_ctx;
  AVCodecContext *enc_ctx;
  AVCodec *encoder;
  const AVCodecDescriptor *codec_desc;
  enum AVCodecID codec_id;
  int ret;
  int i;

  ctx->ofmt_ctx = NULL;
  avformat_alloc_output_context2(&ctx->ofmt_ctx, NULL, ctx->out_format, NULL);
  if (!ctx->ofmt_ctx)
    {
      DPRINTF(E_LOG, L_XCODE, "Could not create output context\n");
      return -1;
    }

  ctx->obuf = evbuffer_new();
  if (!ctx->obuf)
    {
      DPRINTF(E_LOG, L_XCODE, "Could not create output evbuffer\n");
      goto out_fail_evbuf;
    }

  ctx->ofmt_ctx->pb = avio_evbuffer_open(ctx->obuf);
  if (!ctx->ofmt_ctx->pb)
    {
      DPRINTF(E_LOG, L_XCODE, "Could not create output avio pb\n");
      goto out_fail_pb;
    }

  for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++)
    { 
      if (ctx->stream_map[i] < 0)
        continue;

      in_stream = ctx->ifmt_ctx->streams[i];
      out_stream = avformat_new_stream(ctx->ofmt_ctx, NULL);
      if (!out_stream)
	{
	  DPRINTF(E_LOG, L_XCODE, "Failed allocating output stream\n");
	  goto out_fail_stream;
        }

      ctx->stream_map[i] = out_stream->index;

      dec_ctx = in_stream->codec;
      enc_ctx = out_stream->codec;

      // TODO Enough to just remux subtitles?
      if (dec_ctx->codec_type == AVMEDIA_TYPE_SUBTITLE)
	{
	  avcodec_copy_context(enc_ctx, dec_ctx);
	  continue;
	}

      if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
	codec_id = ctx->out_audio_codec;
      else if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
	codec_id = ctx->out_video_codec;
      else
	continue;

      codec_desc = avcodec_descriptor_get(codec_id);

      encoder = avcodec_find_encoder(codec_id);
      if (!encoder)
	{
	  DPRINTF(E_LOG, L_XCODE, "Necessary encoder (%s) for input stream %u not found\n", codec_desc->name, i);
	  goto out_fail_stream;
	}

      if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
	{
	  enc_ctx->sample_rate = ctx->out_sample_rate;
	  enc_ctx->channel_layout = ctx->out_channel_layout;
	  enc_ctx->channels = ctx->out_channels;
	  enc_ctx->sample_fmt = ctx->out_sample_format;
	  enc_ctx->time_base = (AVRational){1, ctx->out_sample_rate};
	}
      else
	{
	  enc_ctx->height = ctx->out_video_height;
	  enc_ctx->width = ctx->out_video_width;
	  enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio; //FIXME
	  enc_ctx->pix_fmt = avcodec_find_best_pix_fmt_of_list(encoder->pix_fmts, dec_ctx->pix_fmt, 1, NULL);
	  enc_ctx->time_base = dec_ctx->time_base;
	}

      ret = avcodec_open2(enc_ctx, encoder, NULL);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot open encoder (%s) for input stream #%u\n", codec_desc->name, i);
	  goto out_fail_codec;
	}

      if (ctx->ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
	enc_ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }

  // Notice, this will not write WAV header (so we do that manually)
  ret = avformat_write_header(ctx->ofmt_ctx, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Error occurred when writing header to output buffer\n");
      goto out_fail_write;
    }

  return 0;

 out_fail_write:
 out_fail_codec:
  for (i = 0; i < ctx->ofmt_ctx->nb_streams; i++)
    {
      enc_ctx = ctx->ofmt_ctx->streams[i]->codec;
      if (enc_ctx)
	avcodec_close(enc_ctx);
    }
 out_fail_stream:
  avio_evbuffer_close(ctx->ofmt_ctx->pb);
 out_fail_pb:
  evbuffer_free(ctx->obuf);
 out_fail_evbuf:
  avformat_free_context(ctx->ofmt_ctx);

  return -1;
}

static void
close_output(struct transcode_ctx *ctx)
{
  AVCodecContext *enc_ctx;
  int i;

  for (i = 0; i < ctx->ofmt_ctx->nb_streams; i++)
    {
      enc_ctx = ctx->ofmt_ctx->streams[i]->codec;
      if (enc_ctx)
	avcodec_close(enc_ctx);
    }

  avio_evbuffer_close(ctx->ofmt_ctx->pb);
  evbuffer_free(ctx->obuf);
  avformat_free_context(ctx->ofmt_ctx);
}

#ifdef HAVE_LIBAVFILTER
static int
open_filter(struct filter_ctx *filter_ctx, AVCodecContext *dec_ctx, AVCodecContext *enc_ctx, const char *filter_spec)
{
  AVFilter *buffersrc = NULL;
  AVFilter *buffersink = NULL;
  AVFilterContext *buffersrc_ctx = NULL;
  AVFilterContext *buffersink_ctx = NULL;
  AVFilterInOut *outputs = avfilter_inout_alloc();
  AVFilterInOut *inputs  = avfilter_inout_alloc();
  AVFilterGraph *filter_graph = avfilter_graph_alloc();
  char args[512];
  int ret;

  if (!outputs || !inputs || !filter_graph)
    {
      DPRINTF(E_LOG, L_XCODE, "Out of memory for filter_graph, input or output\n");
      goto out_fail;
    }

  if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
    {
      buffersrc = avfilter_get_by_name("buffer");
      buffersink = avfilter_get_by_name("buffersink");
      if (!buffersrc || !buffersink)
	{
	  DPRINTF(E_LOG, L_XCODE, "Filtering source or sink element not found\n");
	  goto out_fail;
	}

      snprintf(args, sizeof(args),
               "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
               dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
               dec_ctx->time_base.num, dec_ctx->time_base.den,
               dec_ctx->sample_aspect_ratio.num,
               dec_ctx->sample_aspect_ratio.den);

      ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot create buffer source\n");
	  goto out_fail;
	}

      ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot create buffer sink\n");
	  goto out_fail;
	}

      ret = av_opt_set_bin(buffersink_ctx, "pix_fmts", (uint8_t*)&enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt), AV_OPT_SEARCH_CHILDREN);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot set output pixel format\n");
	  goto out_fail;
	}
    }
  else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
    {
      buffersrc = avfilter_get_by_name("abuffer");
      buffersink = avfilter_get_by_name("abuffersink");
      if (!buffersrc || !buffersink)
	{
	  DPRINTF(E_LOG, L_XCODE, "Filtering source or sink element not found\n");
	  ret = AVERROR_UNKNOWN;
	  goto out_fail;
	}

      if (!dec_ctx->channel_layout)
	dec_ctx->channel_layout = av_get_default_channel_layout(dec_ctx->channels);

      snprintf(args, sizeof(args),
               "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
               dec_ctx->time_base.num, dec_ctx->time_base.den, dec_ctx->sample_rate,
               av_get_sample_fmt_name(dec_ctx->sample_fmt),
               dec_ctx->channel_layout);

      ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot create audio buffer source\n");
	  goto out_fail;
	}

      ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot create audio buffer sink\n");
	  goto out_fail;
	}

      ret = av_opt_set_bin(buffersink_ctx, "sample_fmts",
                           (uint8_t*)&enc_ctx->sample_fmt, sizeof(enc_ctx->sample_fmt), AV_OPT_SEARCH_CHILDREN);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot set output sample format\n");
	  goto out_fail;
	}

      ret = av_opt_set_bin(buffersink_ctx, "channel_layouts",
                           (uint8_t*)&enc_ctx->channel_layout, sizeof(enc_ctx->channel_layout), AV_OPT_SEARCH_CHILDREN);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot set output channel layout\n");
	  goto out_fail;
	}

      ret = av_opt_set_bin(buffersink_ctx, "sample_rates",
                           (uint8_t*)&enc_ctx->sample_rate, sizeof(enc_ctx->sample_rate), AV_OPT_SEARCH_CHILDREN);
      if (ret < 0)
	{
          DPRINTF(E_LOG, L_XCODE, "Cannot set output sample rate\n");
	  goto out_fail;
	}
    }
  else
    {
      DPRINTF(E_LOG, L_XCODE, "Bug! Unknown type passed to filter graph init\n");
      goto out_fail;
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
  if (!outputs->name || !inputs->name)
    {
      DPRINTF(E_LOG, L_XCODE, "Out of memory for outputs/inputs\n");
      goto out_fail;
    }

  ret = avfilter_graph_parse_ptr(filter_graph, filter_spec, &inputs, &outputs, NULL);
  if (ret < 0)
    goto out_fail;

  ret = avfilter_graph_config(filter_graph, NULL);
  if (ret < 0)
    goto out_fail;

  /* Fill filtering context */
  filter_ctx->buffersrc_ctx = buffersrc_ctx;
  filter_ctx->buffersink_ctx = buffersink_ctx;
  filter_ctx->filter_graph = filter_graph;

  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);

  return 0;

 out_fail:
  avfilter_graph_free(&filter_graph);
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);

  return -1;
}
#else
static int
open_filter(struct filter_ctx *filter_ctx, AVCodecContext *dec_ctx, AVCodecContext *enc_ctx, const char *filter_spec)
{

  AVFilter *buffersrc = NULL;
  AVFilter *format = NULL;
  AVFilter *buffersink = NULL;
  AVFilterContext *buffersrc_ctx = NULL;
  AVFilterContext *format_ctx = NULL;
  AVFilterContext *buffersink_ctx = NULL;
  AVFilterGraph *filter_graph = avfilter_graph_alloc();
  char args[512];
  int ret;

  if (!filter_graph)
    {
      DPRINTF(E_LOG, L_XCODE, "Out of memory for filter_graph\n");
      goto out_fail;
    }

  if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
    {
      buffersrc = avfilter_get_by_name("buffer");
      format = avfilter_get_by_name("format");
      buffersink = avfilter_get_by_name("buffersink");
      if (!buffersrc || !format || !buffersink)
	{
	  DPRINTF(E_LOG, L_XCODE, "Filtering source, format or sink element not found\n");
	  goto out_fail;
	}

      snprintf(args, sizeof(args),
               "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
               dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
               dec_ctx->time_base.num, dec_ctx->time_base.den,
               dec_ctx->sample_aspect_ratio.num,
               dec_ctx->sample_aspect_ratio.den);

      ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot create buffer source\n");
	  goto out_fail;
	}

      snprintf(args, sizeof(args),
               "pix_fmt=%d",
               enc_ctx->pix_fmt);

      ret = avfilter_graph_create_filter(&format_ctx, format, "format", args, NULL, filter_graph);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot create format filter\n");
	  goto out_fail;
	}

      ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot create buffer sink\n");
	  goto out_fail;
	}
    }
  else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
    {
      buffersrc = avfilter_get_by_name("abuffer");
      format = avfilter_get_by_name("aformat");
      buffersink = avfilter_get_by_name("abuffersink");
      if (!buffersrc || !format || !buffersink)
	{
	  DPRINTF(E_LOG, L_XCODE, "Filtering source, format or sink element not found\n");
	  ret = AVERROR_UNKNOWN;
	  goto out_fail;
	}

      if (!dec_ctx->channel_layout)
	dec_ctx->channel_layout = av_get_default_channel_layout(dec_ctx->channels);

      snprintf(args, sizeof(args),
               "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
               dec_ctx->time_base.num, dec_ctx->time_base.den, dec_ctx->sample_rate,
               av_get_sample_fmt_name(dec_ctx->sample_fmt),
               dec_ctx->channel_layout);

      ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot create audio buffer source\n");
	  goto out_fail;
	}

      snprintf(args, sizeof(args),
               "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%"PRIx64,
               av_get_sample_fmt_name(enc_ctx->sample_fmt), enc_ctx->sample_rate,
               enc_ctx->channel_layout);

      ret = avfilter_graph_create_filter(&format_ctx, format, "format", args, NULL, filter_graph);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot create audio format filter\n");
	  goto out_fail;
	}

      ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_XCODE, "Cannot create audio buffer sink\n");
	  goto out_fail;
	}
    }
  else
    {
      DPRINTF(E_LOG, L_XCODE, "Bug! Unknown type passed to filter graph init\n");
      goto out_fail;
    }

  ret = avfilter_link(buffersrc_ctx, 0, format_ctx, 0);
  if (ret >= 0)
    ret = avfilter_link(format_ctx, 0, buffersink_ctx, 0);
  if (ret < 0)
    DPRINTF(E_LOG, L_XCODE, "Error connecting filters\n");

  ret = avfilter_graph_config(filter_graph, NULL);
  if (ret < 0)
    goto out_fail;

  /* Fill filtering context */
  filter_ctx->buffersrc_ctx = buffersrc_ctx;
  filter_ctx->buffersink_ctx = buffersink_ctx;
  filter_ctx->filter_graph = filter_graph;

  return 0;

 out_fail:
  avfilter_graph_free(&filter_graph);

  return -1;
}
#endif

static int
open_filters(struct transcode_ctx *ctx)
{
  AVCodecContext *dec_ctx, *enc_ctx;
  const char *filter_spec;
  int i;
  int stream_nb;
  int ret;

  ctx->filter_ctx = av_malloc_array(ctx->ifmt_ctx->nb_streams, sizeof(*ctx->filter_ctx));
  if (!ctx->filter_ctx)
    {
      DPRINTF(E_LOG, L_XCODE, "Out of memory for outputs/inputs\n");
      return -1;
    }

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

      ret = open_filter(&ctx->filter_ctx[i], dec_ctx, enc_ctx, filter_spec);
      if (ret < 0)
	goto out_fail;
    }

  return 0;

 out_fail:
  for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++)
    {
      if (ctx->filter_ctx && ctx->filter_ctx[i].filter_graph)
	avfilter_graph_free(&ctx->filter_ctx[i].filter_graph);
    }
  av_free(ctx->filter_ctx);

  return -1;
}

static void
close_filters(struct transcode_ctx *ctx)
{
  int i;

  for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++)
    {
      if (ctx->filter_ctx && ctx->filter_ctx[i].filter_graph)
	avfilter_graph_free(&ctx->filter_ctx[i].filter_graph);
    }
  av_free(ctx->filter_ctx);
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

  // Encode filtered frame
  enc_pkt.data = NULL;
  enc_pkt.size = 0;
  av_init_packet(&enc_pkt);
  ret = enc_func(out_stream->codec, &enc_pkt, filt_frame, got_frame);
  if (ret < 0)
    return -1;
  if (!(*got_frame))
    return 0;

  // Prepare packet for muxing
  enc_pkt.stream_index = stream_nb;

  // This "wonderful" peace of code makes sure that the timestamp never decreases,
  // even if the user seeked backwards. The muxer will not accept decreasing
  // timestamps
  enc_pkt.pts += ctx->offset_pts[stream_nb];
  if (enc_pkt.pts < ctx->prev_pts[stream_nb])
    {
      ctx->offset_pts[stream_nb] += ctx->prev_pts[stream_nb] - enc_pkt.pts;
      enc_pkt.pts = ctx->prev_pts[stream_nb];
    }
  ctx->prev_pts[stream_nb] = enc_pkt.pts;
  enc_pkt.dts = enc_pkt.pts; //FIXME

  av_packet_rescale_ts(&enc_pkt, out_stream->codec->time_base, out_stream->time_base);

  // Mux encoded frame
  ret = av_interleaved_write_frame(ctx->ofmt_ctx, &enc_pkt);
  return ret;
}

#ifdef HAVE_LIBAVFILTER
static int
filter_encode_write_frame(struct transcode_ctx *ctx, AVFrame *frame, unsigned int stream_index)
{
  AVFrame *filt_frame;
  int ret;

  // Push the decoded frame into the filtergraph
  ret = av_buffersrc_add_frame_flags(ctx->filter_ctx[stream_index].buffersrc_ctx, frame, 0);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Error while feeding the filtergraph\n");
      return -1;
    }

  // Pull filtered frames from the filtergraph
  while (1)
    {
      filt_frame = av_frame_alloc();
      if (!filt_frame)
	{
	  DPRINTF(E_LOG, L_XCODE, "Out of memory for filt_frame\n");
	  return -1;
	}

      ret = av_buffersink_get_frame(ctx->filter_ctx[stream_index].buffersink_ctx, filt_frame);
      if (ret < 0)
	{
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
      av_frame_free(&filt_frame);
      if (ret < 0)
	break;
    }

  return ret;
}
#else
static int
filter_encode_write_frame(struct transcode_ctx *ctx, AVFrame *frame, unsigned int stream_index)
{
  AVFilterBufferRef *picref;
  AVCodecContext *enc_ctx;
  AVFrame *filt_frame;
  int stream_nb;
  int ret;

  stream_nb = ctx->stream_map[stream_index];
  enc_ctx = ctx->ofmt_ctx->streams[stream_nb]->codec;

  // Push the decoded frame into the filtergraph
  ret = av_buffersrc_write_frame(ctx->filter_ctx[stream_index].buffersrc_ctx, frame);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_XCODE, "Error while feeding the filtergraph\n");
      return -1;
    }

  // Pull filtered frames from the filtergraph
  while (1)
    {
      filt_frame = av_frame_alloc();
      if (!filt_frame)
	{
	  DPRINTF(E_LOG, L_XCODE, "Out of memory for filt_frame\n");
	  return -1;
	}

      if (enc_ctx->codec_type == AVMEDIA_TYPE_AUDIO && !(enc_ctx->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE))
	ret = av_buffersink_read_samples(ctx->filter_ctx[stream_index].buffersink_ctx, &picref, enc_ctx->frame_size);
      else
	ret = av_buffersink_read(ctx->filter_ctx[stream_index].buffersink_ctx, &picref);

      if (ret < 0)
	{
	  /* if no more frames for output - returns AVERROR(EAGAIN)
	   * if flushed and no more frames for output - returns AVERROR_EOF
	   * rewrite retcode to 0 to show it as normal procedure completion
	   */
	  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
	    ret = 0;
	  av_frame_free(&filt_frame);
	  break;
	}

      avfilter_copy_buf_props(filt_frame, picref);
      ret = encode_write_frame(ctx, filt_frame, stream_index, NULL);
      av_frame_free(&filt_frame);
      avfilter_unref_buffer(picref);
      if (ret < 0)
	break;
    }

  return ret;
}
#endif

static void
flush_encoder(struct transcode_ctx *ctx, unsigned int stream_index)
{
  int stream_nb;
  int ret;
  int got_frame;

  stream_nb = ctx->stream_map[stream_index];
  if (stream_nb < 0)
    return;

  DPRINTF(E_DBG, L_XCODE, "Flushing output stream #%u encoder\n", stream_nb);

  if (!(ctx->ofmt_ctx->streams[stream_nb]->codec->codec->capabilities & CODEC_CAP_DELAY))
    return;

  do
    {
      ret = encode_write_frame(ctx, NULL, stream_index, &got_frame);
    }
  while ((ret == 0) && got_frame);
}

struct transcode_ctx *
transcode_setup(enum transcode_profile profile, struct media_file_info *mfi, off_t *est_size)
{
  struct transcode_ctx *ctx;

  ctx = malloc(sizeof(struct transcode_ctx));
  if (!ctx)
    {
      DPRINTF(E_LOG, L_XCODE, "Out of memory for filt_frame\n");
      goto out_fail_ctx;
    }
  memset(ctx, 0, sizeof(struct transcode_ctx));

  if (init_profile(ctx, profile) < 0)
    goto out_fail_profile;
  if (open_input(ctx, mfi) < 0)
    goto out_fail_input;
  if (open_output(ctx) < 0)
    goto out_fail_output;
  if (open_filters(ctx) < 0)
    goto out_fail_filter;

  ctx->duration = mfi->song_length;
  ctx->samples = mfi->sample_count;
  ctx->icy_interval = METADATA_ICY_INTERVAL * ctx->out_channels * ctx->out_byte_depth * ctx->out_sample_rate;

  if (profile == XCODE_PCM16_HEADER)
    {
      ctx->wavhdr = 1;
      make_wav_header(ctx, est_size);
    }

  return ctx;

 out_fail_filter:
  close_output(ctx);
 out_fail_output:
  close_input(ctx);
 out_fail_input:
 out_fail_profile:
  free(ctx);
 out_fail_ctx:

  return NULL;
}

int
transcode(struct transcode_ctx *ctx, struct evbuffer *evbuf, int wanted, int *icy_timer)
{
  AVPacket packet = { .data = NULL, .size = 0 };
  AVStream *in_stream, *out_stream;
  AVFrame *frame = NULL;
  char errbuf[128];
  int stream_nb;
  int processed;
  int got_frame;
  int retry;
  int ret;

  if (ctx->wavhdr && (ctx->offset == 0))
    {
      evbuffer_add(evbuf, ctx->header, sizeof(ctx->header));
      processed += sizeof(ctx->header);
      ctx->offset += sizeof(ctx->header);
    }

  ret = 0;
  retry = 0;
  processed = 0;
  while (processed < wanted)
    {
      if (ctx->resume)
	{
          av_copy_packet(&packet, &ctx->seek_packet);
	  ctx->resume = 0;
	}
      else if ((ret = av_read_frame(ctx->ifmt_ctx, &packet)) < 0)
	break;

      stream_nb = ctx->stream_map[packet.stream_index];
      if (stream_nb < 0)
	{
	  av_free_packet(&packet);
	  continue;
	}

      in_stream = ctx->ifmt_ctx->streams[packet.stream_index];
      out_stream = ctx->ofmt_ctx->streams[stream_nb];

      if (ctx->filter_ctx[packet.stream_index].filter_graph)
	{
	  frame = av_frame_alloc();
	  if (!frame)
	    {
	      ret = AVERROR(ENOMEM);
	      break;
	    }

	  av_packet_rescale_ts(&packet, in_stream->time_base, in_stream->codec->time_base);

	  if (in_stream->codec->codec_type == AVMEDIA_TYPE_AUDIO)
	    ret = avcodec_decode_audio4(in_stream->codec, frame, &got_frame, &packet);
	  else
	    ret = avcodec_decode_video2(in_stream->codec, frame, &got_frame, &packet);

	  if (ret >= 0)
	    retry = 0;
	  else if (!retry)
	    {
	      av_frame_free(&frame);
	      av_free_packet(&packet);

	      DPRINTF(E_WARN, L_XCODE, "Couldn't decode packet, let's try the next one\n");
	      retry = 1;
	      continue;
	    }
	  else
	    {
	      av_frame_free(&frame);

	      DPRINTF(E_LOG, L_XCODE, "Couldn't decode packet after two attempts, giving up\n");
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
	  /* remux this frame without reencoding */
	  av_packet_rescale_ts(&packet, in_stream->time_base, out_stream->time_base);

	  ret = av_interleaved_write_frame(ctx->ofmt_ctx, &packet);
	  if (ret < 0)
	    goto end;
	}

      av_free_packet(&packet);

      processed += evbuffer_get_length(ctx->obuf);

      evbuffer_add_buffer(evbuf, ctx->obuf);
    }

  ctx->offset += processed;
  *icy_timer = (ctx->offset % ctx->icy_interval < processed);

  av_free_packet(&packet);
  av_frame_free(&frame);

end:
  if (ret < 0)
    {
      av_strerror(ret, errbuf, sizeof(errbuf));
      DPRINTF(E_LOG, L_XCODE, "Error occurred: %s\n", errbuf);
    }
  else
    ret = processed;

  return ret;
}

void
transcode_cleanup(struct transcode_ctx *ctx)
{
  int i;

  // Flush filters and encoders
  for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++)
    {
      if (ctx->stream_map[i] < 0)
	continue;
      if (!ctx->filter_ctx[i].filter_graph)
	continue;
      filter_encode_write_frame(ctx, NULL, i);
      flush_encoder(ctx, i);
    }

  av_write_trailer(ctx->ofmt_ctx);

  close_filters(ctx);
  close_output(ctx);
  close_input(ctx);

  free(ctx);
}

int
transcode_seek(struct transcode_ctx *ctx, int ms)
{
  int64_t start_time;
  int64_t target_pts;
  int64_t got_pts;
  int stream_nb;
  int got_ms;
  int flags;
  int ret;
  int i;

  start_time = ctx->audio_stream->start_time;

  target_pts = ms;
  target_pts = target_pts * AV_TIME_BASE / 1000;
  target_pts = av_rescale_q(target_pts, AV_TIME_BASE_Q, ctx->audio_stream->time_base);

  if ((start_time != AV_NOPTS_VALUE) && (start_time > 0))
    target_pts += start_time;

  ret = av_seek_frame(ctx->ifmt_ctx, ctx->audio_stream->index, target_pts, AVSEEK_FLAG_BACKWARD);
  if (ret < 0)
    {
      DPRINTF(E_WARN, L_XCODE, "Could not seek into stream: %s\n", strerror(AVUNERROR(ret)));

      return -1;
    }

  for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++)
    {
      stream_nb = ctx->stream_map[i];
      if (stream_nb < 0)
	continue;

      avcodec_flush_buffers(ctx->ifmt_ctx->streams[i]->codec);
      avcodec_flush_buffers(ctx->ofmt_ctx->streams[stream_nb]->codec);
    }

  // Fast forward until first packet with a timestamp is found
  ctx->audio_stream->codec->skip_frame = AVDISCARD_NONREF;
  flags = 0;
  while (1)
    {
      av_free_packet(&ctx->seek_packet);

      ret = av_read_frame(ctx->ifmt_ctx, &ctx->seek_packet);
      if (ret < 0)
	{
	  DPRINTF(E_WARN, L_XCODE, "Could not read more data while seeking\n");

	  flags = 1;
	  break;
	}

      if (ctx->seek_packet.stream_index != ctx->audio_stream->index)
	continue;

      // Need a pts to return the real position
      if (ctx->seek_packet.pts == AV_NOPTS_VALUE)
	continue;

      break;
    }
  ctx->audio_stream->codec->skip_frame = AVDISCARD_DEFAULT;

  // Error while reading frame above
  if (flags)
    return -1;

  // Tell transcode() to resume with seek_packet
  ctx->resume = 1;

  // Compute position in ms from pts
  got_pts = ctx->seek_packet.pts;

  if ((start_time != AV_NOPTS_VALUE) && (start_time > 0))
    got_pts -= start_time;

  got_pts = av_rescale_q(got_pts, ctx->audio_stream->time_base, AV_TIME_BASE_Q);
  got_ms = got_pts / (AV_TIME_BASE / 1000);

  DPRINTF(E_DBG, L_XCODE, "Seek wanted %d ms, got %d ms\n", ms, got_ms);

  return got_ms;
}

int
transcode_needed(const char *user_agent, const char *client_codecs, char *file_codectype)
{
  char *codectype;
  cfg_t *lib;
  int size;
  int i;

  if (!file_codectype)
    {
      DPRINTF(E_LOG, L_XCODE, "Can't determine transcode status, codec type is unknown\n");
      return -1;
    }

  lib = cfg_getsec(cfg, "library");

  size = cfg_size(lib, "no_transcode");
  if (size > 0)
    {
      for (i = 0; i < size; i++)
	{
	  codectype = cfg_getnstr(lib, "no_transcode", i);

	  if (strcmp(file_codectype, codectype) == 0)
	    return 0; // Codectype is in no_transcode
	}
    }

  size = cfg_size(lib, "force_transcode");
  if (size > 0)
    {
      for (i = 0; i < size; i++)
	{
	  codectype = cfg_getnstr(lib, "force_transcode", i);

	  if (strcmp(file_codectype, codectype) == 0)
	    return 1; // Codectype is in force_transcode
	}
    }

  if (!client_codecs)
    {
      if (user_agent)
	{
	  if (strncmp(user_agent, "iTunes", strlen("iTunes")) == 0)
	    client_codecs = itunes_codecs;
	  else if (strncmp(user_agent, "QuickTime", strlen("QuickTime")) == 0)
	    client_codecs = itunes_codecs; // Use iTunes codecs
	  else if (strncmp(user_agent, "Front%20Row", strlen("Front%20Row")) == 0)
	    client_codecs = itunes_codecs; // Use iTunes codecs
	  else if (strncmp(user_agent, "AppleCoreMedia", strlen("AppleCoreMedia")) == 0)
	    client_codecs = itunes_codecs; // Use iTunes codecs
	  else if (strncmp(user_agent, "Roku", strlen("Roku")) == 0)
	    client_codecs = roku_codecs;
	  else if (strncmp(user_agent, "Hifidelio", strlen("Hifidelio")) == 0)
	    /* Allegedly can't transcode for Hifidelio because their
	     * HTTP implementation doesn't honour Connection: close.
	     * At least, that's why mt-daapd didn't do it.
	     */
	    return 0;
	}
    }
  else
    DPRINTF(E_DBG, L_XCODE, "Client advertises codecs: %s\n", client_codecs);

  if (!client_codecs)
    {
      DPRINTF(E_DBG, L_XCODE, "Could not identify client, using default codectype set\n");
      client_codecs = default_codecs;
    }

  if (strstr(client_codecs, file_codectype))
    {
      DPRINTF(E_DBG, L_XCODE, "Codectype supported by client, no transcoding needed\n");
      return 0;
    }

  DPRINTF(E_DBG, L_XCODE, "Will transcode\n");
  return 1;
}

struct http_icy_metadata *
transcode_metadata(struct transcode_ctx *ctx, int *changed)
{
  struct http_icy_metadata *m;

  if (!ctx->ifmt_ctx)
    return NULL;

  m = http_icy_metadata_get(ctx->ifmt_ctx, 1);
  if (!m)
    return NULL;

  *changed = (m->hash != ctx->icy_hash);

  ctx->icy_hash = m->hash;

  return m;
}

char *
transcode_metadata_artwork_url(struct transcode_ctx *ctx)
{
  struct http_icy_metadata *m;
  char *artwork_url;

  if (!ctx->ifmt_ctx || !ctx->ifmt_ctx->filename)
    return NULL;

  artwork_url = NULL;

  m = http_icy_metadata_get(ctx->ifmt_ctx, 1);
  if (m && m->artwork_url)
    artwork_url = strdup(m->artwork_url);

  if (m)
    http_icy_metadata_free(m, 0);

  return artwork_url;
}

