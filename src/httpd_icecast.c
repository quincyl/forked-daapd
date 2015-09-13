/*
 * Copyright (C) 2015 Espen Jürgensen <espenjurgensen@gmail.com>
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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <uninorm.h>
#include <unistd.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/http.h>

#include "logger.h"
#include "conffile.h"
#include "transcode.h"
#include "player.h"
#include "httpd.h"
#include "httpd_icecast.h"

/* httpd event base, from httpd.c */
extern struct event_base *evbase_httpd;

// Seconds between sending silence when player is idle
// (to prevent client from hanging up)
#define ICECAST_SILENCE_INTERVAL 1
// Buffer size for transmitting from player to httpd thread
#define ICECAST_RAWBUF_SIZE (STOB(AIRTUNES_V2_PACKET_SAMPLES))
// Should prevent that we keep transcoding to dead connections
#define ICECAST_CONNECTION_TIMEOUT 60

// Linked list of Icecast requests
struct icecast_request {
  struct evhttp_request *req;
  struct icecast_request *next;
};
static struct icecast_request *icecast_requests;
static struct timeval icecast_silence_tv = { ICECAST_SILENCE_INTERVAL, 0 };

// Input buffer, output buffer and encoding ctx for transcode
static uint8_t icecast_rawbuf[ICECAST_RAWBUF_SIZE];
static struct encode_ctx *icecast_encode_ctx;
static struct evbuffer *icecast_encoded_data;

// Used for pushing events and data from the player
static struct event *icecastev;
static int icecast_pipe[2];


static void
icecast_fail_cb(struct evhttp_connection *evcon, void *arg)
{
  struct icecast_request *this;
  struct icecast_request *ir;
  struct icecast_request *prev;

  this = (struct icecast_request *)arg;

  DPRINTF(E_WARN, L_ICECAST, "Connection failed; stopping mp3 stream to client\n");

  prev = NULL;
  for (ir = icecast_requests; ir; ir = ir->next)
    {
      if (ir->req == this->req)
	break;

      prev = ir;
    }

  if (!prev)
    icecast_requests = ir->next;
  else
    prev->next = ir->next;

  free(ir);

  if (!icecast_requests)
    {
      DPRINTF(E_INFO, L_ICECAST, "No more clients, will stop streaming\n");
      player_icecast_stop();
    }
}

static void
icecast_send_cb(evutil_socket_t fd, short event, void *arg)
{
  struct icecast_request *ir;
  struct evbuffer *evbuf;
  struct decoded_frame *decoded;
  uint8_t *buf;
  int len;
  int ret;

  if (!icecast_requests)
    return;

  // Callback from player (EV_READ)
  if (event & EV_READ)
    {
      ret = read(icecast_pipe[0], &icecast_rawbuf, ICECAST_RAWBUF_SIZE);
      if (ret < 0)
	return;
    }
  // Player is idle, so event timed out and we will send some mp3 silence
  else
    {
      memset(&icecast_rawbuf, 0, ICECAST_RAWBUF_SIZE);
    }

  decoded = transcode_raw2frame(icecast_rawbuf, ICECAST_RAWBUF_SIZE);
  if (!decoded)
    {
      DPRINTF(E_LOG, L_ICECAST, "Could not convert raw PCM to frame\n");
      return;
    }

  ret = transcode_encode(icecast_encoded_data, decoded, icecast_encode_ctx);
  transcode_decoded_free(decoded);
  if (ret < 0)
    return;

  len = evbuffer_get_length(icecast_encoded_data);

  // Send data
  evbuf = evbuffer_new();
  for (ir = icecast_requests; ir; ir = ir->next)
    {
      if (ir->next)
	{
	  buf = evbuffer_pullup(icecast_encoded_data, -1);
	  evbuffer_add(evbuf, buf, len);
	  evhttp_send_reply_chunk(ir->req, evbuf);
	}
      else
	evhttp_send_reply_chunk(ir->req, icecast_encoded_data);
    }
  evbuffer_free(evbuf);
}

// Thread: player
static int
icecast_cb(uint8_t *rawbuf, size_t size)
{
  if (size != ICECAST_RAWBUF_SIZE)
    {
      DPRINTF(E_LOG, L_ICECAST, "Bug! Buffer size in icecast_cb does not equal input from player\n");
      return -1;
    }

  if (write(icecast_pipe[1], rawbuf, size) < 0)
    return -1;

  return 0;
}

int
icecast_is_request(struct evhttp_request *req, char *uri)
{
  char *ptr;

  ptr = strrchr(uri, '/');
  if (!ptr || (strcasecmp(ptr, "/stream.mp3") != 0))
    return 0;

  return 1;
}

int
icecast_request(struct evhttp_request *req)
{
  struct icecast_request *ir;
  struct evhttp_connection *evcon;
  struct evkeyvalq *output_headers;
  cfg_t *lib;
  const char *name;
  char *address;
  ev_uint16_t port;

  if (!icecast_encode_ctx)
    {
      DPRINTF(E_LOG, L_ICECAST, "Got mp3 stream request, but cannot encode to mp3\n");

      evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
      return -1;
    }

  evcon = evhttp_request_get_connection(req);
  evhttp_connection_get_peer(evcon, &address, &port);

  DPRINTF(E_INFO, L_ICECAST, "Beginning mp3 streaming to %s:%d\n", address, (int)port);

  lib = cfg_getsec(cfg, "library");
  name = cfg_getstr(lib, "name");

  output_headers = evhttp_request_get_output_headers(req);
  evhttp_add_header(output_headers, "Content-Type", "audio/mpeg");
  evhttp_add_header(output_headers, "Server", "forked-daapd/" VERSION);
  evhttp_add_header(output_headers, "Cache-Control", "no-cache");
  evhttp_add_header(output_headers, "Pragma", "no-cache");
  evhttp_add_header(output_headers, "Expires", "Mon, 31 Aug 2015 06:00:00 GMT");
  evhttp_add_header(output_headers, "icy-name", name);

  // TODO ICY metaint
  evhttp_send_reply_start(req, HTTP_OK, "OK");

  ir = malloc(sizeof(struct icecast_request));
  if (!ir)
    {
      DPRINTF(E_LOG, L_ICECAST, "Out of memory for icecast request\n");

      evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
      return -1;
    }

  ir->req = req;
  ir->next = icecast_requests;
  icecast_requests = ir;

  evhttp_connection_set_timeout(evcon, ICECAST_CONNECTION_TIMEOUT);
  evhttp_connection_set_closecb(evcon, icecast_fail_cb, ir);

  player_icecast_start(icecast_cb);

  return 0;
}

int
icecast_init(void)
{
  struct decode_ctx *decode_ctx;
  int ret;

  decode_ctx = transcode_decode_setup_raw();
  if (!decode_ctx)
    {
      DPRINTF(E_LOG, L_ICECAST, "Could not create decoding context\n");
      return -1;
    }

  icecast_encode_ctx = transcode_encode_setup(decode_ctx, XCODE_MP3, NULL);
  transcode_decode_cleanup(decode_ctx);
  if (!icecast_encode_ctx)
    {
      DPRINTF(E_LOG, L_ICECAST, "Will not be able to stream mp3, libav does not support mp3 encoding\n");
      return -1;
    }

  // Non-blocking because otherwise httpd and player thread may deadlock
  ret = pipe2(icecast_pipe, O_CLOEXEC | O_NONBLOCK);
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_ICECAST, "Could not create pipe: %s\n", strerror(errno));
      goto pipe_fail;
    }

  icecast_encoded_data = evbuffer_new();
  icecastev = event_new(evbase_httpd, icecast_pipe[0], EV_TIMEOUT | EV_READ | EV_PERSIST, icecast_send_cb, NULL);
  if (!icecast_encoded_data || !icecastev)
    {
      DPRINTF(E_LOG, L_ICECAST, "Out of memory for encoded_data or event\n");
      goto event_fail;
    }

  event_add(icecastev, &icecast_silence_tv);

  return 0;

 event_fail:
  close(icecast_pipe[0]);
  close(icecast_pipe[1]);
 pipe_fail:
  transcode_encode_cleanup(icecast_encode_ctx);

  return -1;
}

void
icecast_deinit(void)
{
  struct icecast_request *ir;
  struct icecast_request *next;

  player_icecast_stop();

  event_free(icecastev);

  next = NULL;
  for (ir = icecast_requests; ir; ir = next)
    {
      evhttp_send_reply_end(ir->req);
      next = ir->next;
      free(ir);
    }

  close(icecast_pipe[0]);
  close(icecast_pipe[1]);

  transcode_encode_cleanup(icecast_encode_ctx);
  evbuffer_free(icecast_encoded_data);
}
