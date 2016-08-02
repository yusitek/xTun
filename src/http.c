#include <string.h>
#include <stdlib.h>

#include "http.h"

static uv_buf_t *header_buf = NULL;
static uv_buf_t ok_buf, fail_buf, client_buf;
static uv_write_t header_write_request;

void init_client_header(char *h)
{
    client_buf.base = strdup(h);
    client_buf.len = strlen(h);
    header_buf = &client_buf;
}

void init_server_header()
{
  ok_buf.base = "HTTP/1.1 200 OK\r\nConnection: Keep-Alive\r\n\r\n";
  ok_buf.len = 43;

  fail_buf.base = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
  fail_buf.len  =  45;
}

//static void
//auth_cb(uv_write_t *req, int status) {
    //logger_log(LOG_INFO, "auth send status=%d", status);
//}

int send_client_header(uv_stream_t *stream) 
{
    int rt = 0;
    if (header_buf != NULL) {
        int rc = uv_write(&header_write_request, stream, header_buf, 1, NULL);
        if (rc == 0) {
            rt = 1;
        } else {
            logger_log(LOG_ERR, "Send client auth request header error: %s", uv_strerror(rc));
            exit(1);
        }
    }

    return rt;
}

static int shell_run(char *cmd, char *args) 
{
  char *buf;
  int r;

  buf = malloc(strlen(cmd) + strlen(args) + 8);
  sprintf(buf, "sh %s $'%s'", cmd, args);

  logger_log(LOG_INFO, "executing %s:%s", cmd, args);
  if (0 != (r = system(buf))) {
    free(buf);
    logger_log(LOG_ERR, "script %s returned non-zero return code: %d",  cmd, r);
    return 0;
  }
  free(buf);

  return 1;
}


static void
send_cb(uv_write_t *req, int status) {
    free(req);
}

int http_auth(uv_stream_t *stream,  uint8_t * header)
{
    if (shell_run("auth",  (char *)header)) {
        header_buf = &ok_buf;
    } else {
        header_buf = &fail_buf;
    }

    uv_write_t *req = malloc(sizeof(*req));
    int rc = uv_write(req, stream, header_buf, 1, send_cb);
    if (rc) {
        logger_log(LOG_ERR, "Send http auth header error: %s", uv_strerror(rc));
        free(req);
    }

    return rc;
}

