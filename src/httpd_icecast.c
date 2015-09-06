#include "player.h"

#define ICECAST_RAWBUF_SIZE (STOB(AIRTUNES_V2_PACKET_SAMPLES))

// Linked list of Icecast requests
struct icecast_request {
  struct evhttp_request *req;
  struct icecast_request *next;
};

struct icecast_ctx {
  struct icecast_request *requests;

  struct evbuffer *encoded_data;
  struct encode_ctx *encode_ctx;

  struct spsc_queue *queue;
};

struct icecast_ctx icecast_ctx;
struct event *icecastev;

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
  if (!ptr || (strcasecmp(ptr, "stream.mp3") != 0))
    return 0;

  return 1;
}

static void
icecast_send_cb(evutil_socket_t fd, short event, void *arg)
{
  struct icecast_request *ir;
  struct evbuffer *evbuf;
  struct decoded_frame *decoded;
  int ret;

  if (!pthread_equal(pthread_self(), tid_httpd))
    DPRINTF(E_INFO, L_HTTPD, "OH NO IT IS THE WRONG THREAD\n");

  ret = spsc_queue_pop(icecast_ctx.queue, (void *)&decoded);
  if (ret < 0)
    return; // or spin?

  ret = transcode_encode(icecast_ctx.encoded_data, decoded, icecast_ctx.encode_ctx);
  transcode_decoded_free(decoded);
  if (ret < 0)
    return;

  // Collect data until chunk_size is reached, then send
  if (evbuffer_get_length(icecast_ctx.encoded_data) < STREAM_CHUNK_SIZE)
    return;

  // Send data
  evbuf = evbuffer_new();
  for (ir = icecast_ctx.requests; ir; ir = ir->next)
    {
      evbuffer_add_buffer_reference(evbuf, icecast_ctx.encoded_data);
      evhttp_send_reply_chunk(ir->req, evbuf);
    }
}

// Thread: player
static int
icecast_cb(uint8_t *rawbuf, size_t size)
{
  struct decoded_frame *decoded;

  if (size != ICECAST_RAWBUF_SIZE)
    {
      DPRINTF(E_LOG, L_HTTPD, "Bug! Unexpected buffer size\n");
      return -1;
    }

  decoded = transcode_raw2frame(rawbuf, size);
  if (!decoded)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not convert raw PCM to frame\n");
      return -1;
    }

  // Push a pointer to the decoded audio buffer from the player to the queue
  // and wake the evhttp_base loop
  if (spsc_queue_push(icecast_ctx.queue, decoded) != 0)
    return -1;

  event_active(icecastev, 0, 0);
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

  free(address); // Not clear if we should free this?

  output_headers = evhttp_request_get_output_headers(req);
  evhttp_add_header(output_headers, "Content-Type", "audio/mpeg");
  evhttp_add_header(output_headers, "Server", "forked-daapd/" VERSION);
  evhttp_add_header(output_headers, "Cache-Control", "no-cache");
  evhttp_add_header(output_headers, "Pragma", "no-cache");
  evhttp_add_header(output_headers, "Expires", "Mon, 31 Aug 2015 06:00:00 GMT");
  evhttp_add_header(output_headers, "icy-br", "128");
  evhttp_add_header(output_headers, "icy-name", "Testing FD");

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
  icecast_ctx.encode_ctx = transcode_encode_setup(XCODE_MP3, NULL);
  if (!icecast_ctx.encode_ctx)
    {
      DPRINTF(E_LOG, L_HTTPD, "Icecasting will not be available, libav does not support mp3 encoding\n");
      return -1;
    }

  icecast_ctx.queue = spsc_queue_new();
  if (!icecast_ctx.queue)
    {
      DPRINTF(E_LOG, L_HTTPD, "Out of memory for icecast queue\n");
      return -1;
    }

  icecast_ctx.encoded_data = evbuffer_new();

  icecastev = event_new(evbase_httpd, -1, EV_PERSIST, icecast_send_cb, NULL);
  if (!icecastev)
    {
      DPRINTF(E_LOG, L_HTTPD, "Out of memory for icecast event\n");
      return -1;
    }

  return 0;
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

  transcode_encode_cleanup(icecast_ctx.encode_ctx);
  evbuffer_free(icecast_ctx.encoded_data);
  free(icecast_ctx.queue);
}
