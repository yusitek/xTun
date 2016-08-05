#ifndef HTTP_H
#define HTTP_H

#include "uv.h"
#include "logger.h"

int init_client_header(char *h);
void init_server_header();
int send_client_header(uv_stream_t *stream);
int http_auth(uv_stream_t *stream,  uint8_t * header);


#endif
