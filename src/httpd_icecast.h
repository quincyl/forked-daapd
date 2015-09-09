
#ifndef __HTTPD_ICECAST_H__
#define __HTTPD_ICECAST_H__

#include <event2/http.h>

int
icecast_is_request(struct evhttp_request *req, char *uri);

int
icecast_request(struct evhttp_request *req);

int
icecast_init(void);

void
icecast_deinit(void);

#endif /* !__HTTPD_ICECAST_H__ */
