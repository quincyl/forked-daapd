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
#include "transcode.h"

struct filter_ctx {
  AVFilterContext *buffersink_ctx;
  AVFilterContext *buffersrc_ctx;
  AVFilterGraph *filter_graph;
};

struct transcode_ctx {
  AVFormatContext *ifmt_ctx;
  AVFormatContext *ofmt_ctx;

  struct filter_ctx *filter_ctx;

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
    wav_len = 2 * 2 * ctx->samples;
  else
    wav_len = 2 * 2 * 44100 * (duration / 1000);

  *est_size = wav_len + sizeof(ctx->header);

  memcpy(ctx->header, "RIFF", 4);
  add_le32(ctx->header + 4, 36 + wav_len);
  memcpy(ctx->header + 8, "WAVEfmt ", 8);
  add_le32(ctx->header + 16, 16);
  add_le16(ctx->header + 20, 1);
  add_le16(ctx->header + 22, 2);               /* channels */
  add_le32(ctx->header + 24, 44100);           /* samplerate */
  add_le32(ctx->header + 28, 44100 * 2 * 2);   /* byte rate */
  add_le16(ctx->header + 32, 2 * 2);           /* block align */
  add_le16(ctx->header + 34, 16);              /* bits per sample */
  memcpy(ctx->header + 36, "data", 4);
  add_le32(ctx->header + 40, wav_len);
}

static int open_input(struct transcode_ctx *ctx, const char *path)
{
    int ret;
    unsigned int i;

    ctx->ifmt_ctx = NULL;
    if ((ret = avformat_open_input(&ctx->ifmt_ctx, path, NULL, NULL)) < 0) {
        DPRINTF(E_LOG, L_XCODE, "Cannot open input path\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(ctx->ifmt_ctx, NULL)) < 0) {
        DPRINTF(E_LOG, L_XCODE, "Cannot find stream information\n");
        return ret;
    }

    for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++) {
        AVStream *stream;
        AVCodecContext *codec_ctx;
        stream = ctx->ifmt_ctx->streams[i];
        codec_ctx = stream->codec;
        /* Reencode video & audio and remux subtitles etc. */
        if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            /* Open decoder */
            ret = avcodec_open2(codec_ctx,
                    avcodec_find_decoder(codec_ctx->codec_id), NULL);
            if (ret < 0) {
                DPRINTF(E_LOG, L_XCODE, "Failed to open decoder for stream #%u\n", i);
                return ret;
            }
        }
    }

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
    avformat_alloc_output_context2(&ctx->ofmt_ctx, NULL, NULL, NULL);
    if (!ctx->ofmt_ctx) {
        DPRINTF(E_LOG, L_XCODE, "Could not create output context\n");
        return AVERROR_UNKNOWN;
    }


    for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++) {
        out_stream = avformat_new_stream(ctx->ofmt_ctx, NULL);
        if (!out_stream) {
            DPRINTF(E_LOG, L_XCODE, "Failed allocating output stream\n");
            return AVERROR_UNKNOWN;
        }

        in_stream = ctx->ifmt_ctx->streams[i];
        dec_ctx = in_stream->codec;
        enc_ctx = out_stream->codec;

        if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            /* in this example, we choose transcoding to same codec */
            encoder = avcodec_find_encoder(dec_ctx->codec_id);
            if (!encoder) {
                DPRINTF(E_LOG, L_XCODE, "Necessary encoder not found\n");
                return AVERROR_INVALIDDATA;
            }

            /* In this example, we transcode to same properties (picture size,
             * sample rate etc.). These properties can be changed for output
             * streams easily using filters */
            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                enc_ctx->height = dec_ctx->height;
                enc_ctx->width = dec_ctx->width;
                enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
                /* take first format from list of supported formats */
                enc_ctx->pix_fmt = encoder->pix_fmts[0];
                /* video time_base can be set to whatever is handy and supported by encoder */
                enc_ctx->time_base = dec_ctx->time_base;
            } else {
/*                enc_ctx->sample_rate = dec_ctx->sample_rate;
                enc_ctx->channel_layout = dec_ctx->channel_layout;
                enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
*/
                /* take first format from list of supported formats */
/*                enc_ctx->sample_fmt = encoder->sample_fmts[0];
                enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
*/
                enc_ctx->sample_rate = 44100;
                enc_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
                enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
                /* take first format from list of supported formats */
                enc_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
                enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
            }

            /* Third parameter can be used to pass settings to encoder */
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
            ret = avcodec_copy_context(ctx->ofmt_ctx->streams[i]->codec,
                    ctx->ifmt_ctx->streams[i]->codec);
            if (ret < 0) {
                DPRINTF(E_LOG, L_XCODE, "Copying stream context failed\n");
                return ret;
            }
        }

        if (ctx->ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            enc_ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;

    }

    /* init muxer, write output file header */
/*    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        DPRINTF(E_LOG, L_XCODE, "Error occurred when opening output file\n");
        return ret;
    }
*/

    return 0;
}

static int init_filter(struct filter_ctx *filter_ctx, AVCodecContext *dec_ctx,
        AVCodecContext *enc_ctx, const char *filter_spec)
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

        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                args, NULL, filter_graph);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Cannot create buffer source\n");
            goto end;
        }

        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                NULL, NULL, filter_graph);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Cannot create buffer sink\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "pix_fmts",
                (uint8_t*)&enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt),
                AV_OPT_SEARCH_CHILDREN);
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

        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                NULL, NULL, filter_graph);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Cannot create audio buffer sink\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "sample_fmts",
                (uint8_t*)&enc_ctx->sample_fmt, sizeof(enc_ctx->sample_fmt),
                AV_OPT_SEARCH_CHILDREN);
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

static int init_filters(struct transcode_ctx *ctx)
{
    const char *filter_spec;
    unsigned int i;
    int ret;
    ctx->filter_ctx = av_malloc_array(ctx->ifmt_ctx->nb_streams, sizeof(*ctx->filter_ctx));
    if (!ctx->filter_ctx)
        return AVERROR(ENOMEM);

    for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++) {
        ctx->filter_ctx[i].buffersrc_ctx  = NULL;
        ctx->filter_ctx[i].buffersink_ctx = NULL;
        ctx->filter_ctx[i].filter_graph   = NULL;
        if (!(ctx->ifmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO
                || ctx->ifmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO))
            continue;


        if (ctx->ifmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
            filter_spec = "null"; /* passthrough (dummy) filter for video */
        else
            filter_spec = "anull"; /* passthrough (dummy) filter for audio */
        ret = init_filter(&ctx->filter_ctx[i], ctx->ifmt_ctx->streams[i]->codec,
                ctx->ofmt_ctx->streams[i]->codec, filter_spec);
        if (ret)
            return ret;
    }
    return 0;
}

static int encode_write_frame(struct transcode_ctx *ctx, AVFrame *filt_frame, unsigned int stream_index, int *got_frame) {
    int ret;
    int got_frame_local;
    AVPacket enc_pkt;
    int (*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *) =
        (ctx->ifmt_ctx->streams[stream_index]->codec->codec_type ==
         AVMEDIA_TYPE_VIDEO) ? avcodec_encode_video2 : avcodec_encode_audio2;

    if (!got_frame)
        got_frame = &got_frame_local;

    DPRINTF(E_LOG, L_XCODE, "Encoding frame\n");
    /* encode filtered frame */
    enc_pkt.data = NULL;
    enc_pkt.size = 0;
    av_init_packet(&enc_pkt);
    ret = enc_func(ctx->ofmt_ctx->streams[stream_index]->codec, &enc_pkt,
            filt_frame, got_frame);
    av_frame_free(&filt_frame);
    if (ret < 0)
        return ret;
    if (!(*got_frame))
        return 0;

    /* prepare packet for muxing */
    enc_pkt.stream_index = stream_index;
    av_packet_rescale_ts(&enc_pkt,
                         ctx->ofmt_ctx->streams[stream_index]->codec->time_base,
                         ctx->ofmt_ctx->streams[stream_index]->time_base);

    DPRINTF(E_LOG, L_XCODE, "Muxing frame\n");
    /* mux encoded frame */
    ret = av_interleaved_write_frame(ctx->ofmt_ctx, &enc_pkt);
    return ret;
}

static int filter_encode_write_frame(struct transcode_ctx *ctx, AVFrame *frame, unsigned int stream_index)
{
    int ret;
    AVFrame *filt_frame;

    DPRINTF(E_LOG, L_XCODE, "Pushing decoded frame to filters\n");
    /* push the decoded frame into the filtergraph */
    ret = av_buffersrc_add_frame_flags(ctx->filter_ctx[stream_index].buffersrc_ctx,
            frame, 0);
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
        DPRINTF(E_LOG, L_XCODE, "Pulling filtered frame from filters\n");
        ret = av_buffersink_get_frame(ctx->filter_ctx[stream_index].buffersink_ctx,
                filt_frame);
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
transcode_setup(struct transcode_ctx **nctx, struct media_file_info *mfi, off_t *est_size, int wavhdr)
{
    struct transcode_ctx *ctx;
    int ret;

    ctx = malloc(sizeof(struct transcode_ctx));
    if (!ctx)
	goto end;
    memset(ctx, 0, sizeof(struct transcode_ctx));

    if ((ret = open_input(ctx, mfi->path)) < 0)
        goto end;
    if ((ret = open_output(ctx)) < 0)
        goto end;
    if ((ret = init_filters(ctx)) < 0)
        goto end;

  ctx->duration = mfi->song_length;
  ctx->samples = mfi->sample_count;
  ctx->wavhdr = wavhdr;

  if (wavhdr)
    make_wav_header(ctx, est_size);

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
    AVFrame *frame = NULL;
    enum AVMediaType type;
    unsigned int stream_index;
    unsigned int i;
  int processed;
  int stop;
    int got_frame;
    int (*dec_func)(AVCodecContext *, AVFrame *, int *, const AVPacket *);
  int ret;

  processed = 0;

  if (ctx->wavhdr && (ctx->offset == 0))
    {
      evbuffer_add(evbuf, ctx->header, sizeof(ctx->header));
      processed += sizeof(ctx->header);
      ctx->offset += sizeof(ctx->header);
    }

//  stop = 0;
//  while ((processed < wanted) && !stop)

    /* read all packets */
    while (1) {
        if ((ret = av_read_frame(ctx->ifmt_ctx, &packet)) < 0)
            break;
        stream_index = packet.stream_index;
        type = ctx->ifmt_ctx->streams[packet.stream_index]->codec->codec_type;
        DPRINTF(E_LOG, L_XCODE, "Demuxer gave frame of stream_index %u\n", stream_index);

        if (ctx->filter_ctx[stream_index].filter_graph) {
            DPRINTF(E_LOG, L_XCODE, "Going to reencode&filter the frame\n");
            frame = av_frame_alloc();
            if (!frame) {
                ret = AVERROR(ENOMEM);
                break;
            }
            av_packet_rescale_ts(&packet,
                                 ctx->ifmt_ctx->streams[stream_index]->time_base,
                                 ctx->ifmt_ctx->streams[stream_index]->codec->time_base);
            dec_func = (type == AVMEDIA_TYPE_VIDEO) ? avcodec_decode_video2 :
                avcodec_decode_audio4;
            ret = dec_func(ctx->ifmt_ctx->streams[stream_index]->codec, frame,
                    &got_frame, &packet);
            if (ret < 0) {
                av_frame_free(&frame);
                DPRINTF(E_LOG, L_XCODE, "Decoding failed\n");
                break;
            }

            if (got_frame) {
                frame->pts = av_frame_get_best_effort_timestamp(frame);
                ret = filter_encode_write_frame(ctx, frame, stream_index);
                av_frame_free(&frame);
                if (ret < 0)
                    goto end;
            } else {
                av_frame_free(&frame);
            }
        } else {
            /* remux this frame without reencoding */
            av_packet_rescale_ts(&packet,
                                 ctx->ifmt_ctx->streams[stream_index]->time_base,
                                 ctx->ofmt_ctx->streams[stream_index]->time_base);

            ret = av_interleaved_write_frame(ctx->ofmt_ctx, &packet);
            if (ret < 0)
                goto end;
        }
        av_free_packet(&packet);
    }

    /* flush filters and encoders */
    for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++) {
        /* flush filter */
        if (!ctx->filter_ctx[i].filter_graph)
            continue;
        ret = filter_encode_write_frame(ctx, NULL, i);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Flushing filter failed\n");
            goto end;
        }

        /* flush encoder */
        ret = flush_encoder(ctx, i);
        if (ret < 0) {
            DPRINTF(E_LOG, L_XCODE, "Flushing encoder failed\n");
            goto end;
        }
    }

//    av_write_trailer(ofmt_ctx);

    av_free_packet(&packet);
    av_frame_free(&frame);
end:
    if (ret < 0)
        DPRINTF(E_LOG, L_XCODE, "Error occurred: %s\n", av_err2str(ret));

    return ret ? 1 : 0;
}

void
transcode_cleanup(struct transcode_ctx *ctx)
{
  int i;

    for (i = 0; i < ctx->ifmt_ctx->nb_streams; i++) {
        avcodec_close(ctx->ifmt_ctx->streams[i]->codec);
        if (ctx->ofmt_ctx && ctx->ofmt_ctx->nb_streams > i && ctx->ofmt_ctx->streams[i] && ctx->ofmt_ctx->streams[i]->codec)
            avcodec_close(ctx->ofmt_ctx->streams[i]->codec);
        if (ctx->filter_ctx && ctx->filter_ctx[i].filter_graph)
            avfilter_graph_free(&ctx->filter_ctx[i].filter_graph);
    }
    av_free(ctx->filter_ctx);
    avformat_close_input(&ctx->ifmt_ctx);
//    if (ctx->ofmt_ctx && !(ctx->ofmt_ctx->oformat->flags & AVFMT_NOFILE))
//        avio_closep(&ofmt_ctx->pb);
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

