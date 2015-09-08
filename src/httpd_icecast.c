#include "player.h"

#define ICECAST_RAWBUF_SIZE (STOB(AIRTUNES_V2_PACKET_SAMPLES))

// Linked list of Icecast requests
struct icecast_request {
  struct evhttp_request *req;
  struct icecast_request *next;
};

struct icecast_ctx {
  struct icecast_request *requests;

  uint8_t rawbuf[ICECAST_RAWBUF_SIZE];
  struct evbuffer *encoded_data;
  struct encode_ctx *encode_ctx;
};

static struct icecast_ctx icecast_ctx;
static struct event *icecastev;
static int icecast_pipe[2];

static void
icecast_fail_cb(struct evhttp_connection *evcon, void *arg)
{
  struct icecast_request *this;
  struct icecast_request *ir;
  struct icecast_request *prev;

  this = (struct icecast_request *)arg;

  DPRINTF(E_WARN, L_HTTPD, "Connection failed; stopping Icecasting to client\n");

  prev = NULL;
  for (ir = icecast_ctx.requests; ir; ir = ir->next)
    {
      if (ir->req == this->req)
	break;

      prev = ir;
    }

  if (!prev)
    icecast_ctx.requests = ir->next;
  else
    prev->next = ir->next;

  free(ir);

  if (!icecast_ctx.requests)
    player_icecast_stop();
}

static int
icecast_is_request(struct evhttp_request *req, char *uri)
{
  char *ptr;

  ptr = strrchr(uri, '/');
  if (!ptr || (strcasecmp(ptr, "/stream.mp3") != 0))
    return 0;

  return 1;
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

  ret = read(icecast_pipe[0], &icecast_ctx.rawbuf, ICECAST_RAWBUF_SIZE);
  if (ret < 0)
    return; // or spin?

  decoded = transcode_raw2frame(icecast_ctx.rawbuf, ICECAST_RAWBUF_SIZE);
  if (!decoded)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not convert raw PCM to frame\n");
      return;
    }

  ret = transcode_encode(icecast_ctx.encoded_data, decoded, icecast_ctx.encode_ctx);
  transcode_decoded_free(decoded);
  if (ret < 0)
    return;

  // Collect data until chunk_size is reached, then send
  len = evbuffer_get_length(icecast_ctx.encoded_data);
  if (len < STREAM_CHUNK_SIZE)
    return;

  // Send data
  evbuf = evbuffer_new();
  for (ir = icecast_ctx.requests; ir; ir = ir->next)
    {
      if (ir->next)
	{
	  buf = evbuffer_pullup(icecast_ctx.encoded_data, -1);
	  evbuffer_add(evbuf, buf, len);
	  evhttp_send_reply_chunk(ir->req, evbuf);
	}
      else
	evhttp_send_reply_chunk(ir->req, icecast_ctx.encoded_data);
    }
  evbuffer_free(evbuf);
}

// Thread: player
static int
icecast_cb(uint8_t *rawbuf, size_t size)
{
  if (size != ICECAST_RAWBUF_SIZE)
    {
      DPRINTF(E_LOG, L_HTTPD, "Bug! Buffer size in icecast_cb must be adjusted\n");
      return -1;
    }

  if (write(icecast_pipe[1], rawbuf, size) < 0)
    return -1;

  return 0;
}

static int
icecast_request(struct evhttp_request *req)
{
  struct icecast_request *ir;
  struct evhttp_connection *evcon;
  struct evkeyvalq *output_headers;
  char *address;
  ev_uint16_t port;

  if (!icecast_ctx.encode_ctx)
    {
      DPRINTF(E_LOG, L_HTTPD, "Got Icecast request, but cannot encode to mp3\n");

      evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
      return -1;
    }

  evcon = evhttp_request_get_connection(req);
  evhttp_connection_get_peer(evcon, &address, &port);

  DPRINTF(E_INFO, L_HTTPD, "Beginning Icecast streaming to %s:%d\n", address, (int)port);

  output_headers = evhttp_request_get_output_headers(req);
  evhttp_add_header(output_headers, "Content-Type", "audio/mpeg");
  evhttp_add_header(output_headers, "Server", "forked-daapd/" VERSION);
  evhttp_add_header(output_headers, "Cache-Control", "no-cache");
  evhttp_add_header(output_headers, "Pragma", "no-cache");
  evhttp_add_header(output_headers, "Expires", "Mon, 31 Aug 2015 06:00:00 GMT");
  evhttp_add_header(output_headers, "icy-br", "128");
  evhttp_add_header(output_headers, "icy-name", "forked-daapd");
  evhttp_add_header(output_headers, "Connection", "Keep-Alive"); //TODO ?

  // TODO ICY metaint
  evhttp_send_reply_start(req, HTTP_OK, "OK");

  ir = malloc(sizeof(struct icecast_request));
  if (!ir)
    {
      DPRINTF(E_LOG, L_HTTPD, "Out of memory for icecast request\n");

      evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
      return -1;
    }

  ir->req = req;
  ir->next = icecast_ctx.requests;
  icecast_ctx.requests = ir;

  evhttp_connection_set_closecb(evcon, icecast_fail_cb, ir);

  player_icecast_start(icecast_cb);

  return 0;
}

static int
icecast_init(void)
{
  struct decode_ctx *decode_ctx;
  int ret;

  decode_ctx = transcode_decode_setup_raw();
  if (!decode_ctx)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not create Icecast decoding context\n");
      return -1;
    }

  icecast_ctx.encode_ctx = transcode_encode_setup(decode_ctx, XCODE_MP3, NULL);
  transcode_decode_cleanup(decode_ctx);
  if (!icecast_ctx.encode_ctx)
    {
      DPRINTF(E_LOG, L_HTTPD, "Icecasting will not be available, libav does not support mp3 encoding\n");
      return -1;
    }

# if defined(__linux__)
  ret = pipe2(icecast_pipe, O_CLOEXEC);
# else
  ret = pipe(icecast_pipe);
# endif
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_HTTPD, "Could not create Icecast pipe: %s\n", strerror(errno));
      goto pipe_fail;
    }

  icecast_ctx.encoded_data = evbuffer_new();
  icecastev = event_new(evbase_httpd, icecast_pipe[0], EV_READ | EV_PERSIST, icecast_send_cb, NULL);
  if (!icecast_ctx.encoded_data || !icecastev)
    {
      DPRINTF(E_LOG, L_HTTPD, "Out of memory for icecast encoded_data or event\n");
      goto event_fail;
    }

  event_add(icecastev, NULL);

  return 0;

 event_fail:
  close(icecast_pipe[0]);
  close(icecast_pipe[1]);
 pipe_fail:
  transcode_encode_cleanup(icecast_ctx.encode_ctx);

  return -1;
}

static void
icecast_deinit(void)
{
  struct icecast_request *ir;
  struct icecast_request *next;

  player_icecast_stop();

  event_free(icecastev);

  next = NULL;
  for (ir = icecast_ctx.requests; ir; ir = next)
    {
      evhttp_send_reply_end(ir->req);
      next = ir->next;
      free(ir);
    }

  close(icecast_pipe[0]);
  close(icecast_pipe[1]);

  transcode_encode_cleanup(icecast_ctx.encode_ctx);
  evbuffer_free(icecast_ctx.encoded_data);
}
