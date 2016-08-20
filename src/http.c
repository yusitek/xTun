#include <string.h>
#include <stdlib.h>

#include "http.h"

static uv_buf_t *header_buf = NULL;
static char * http_header = NULL;
static uv_buf_t ok_buf, fail_buf, client_buf;
static uv_write_t header_write_request;



static int read_http_header(char *fname)
{
	FILE *fp;
    int ret = 0;

    fp = fopen(fname, "rb");
    if(!fp) {
        logger_log(LOG_ERR, "Error reading Http Header file `%s': %s",
                   fname, strerror(errno));
        free(fname);
    } else {
        int status = 0;
        status = fseek(fp, 0, SEEK_END);
        if (status != 0) {
            goto closefile;
        }

        long fsize = ftell(fp);
        status = fseek(fp, 0, SEEK_SET);  //rewind(fp);
        if (status != 0) {
            goto closefile;
        }

        http_header = malloc(fsize + 1); //calloc( 1, lSize+1 );
        if( http_header ) {
             status = fread(http_header, fsize, 1, fp);
             if ( ferror( fp ) == 0 ) {
                //if (status == fsize) {
                http_header[fsize] = 0;
                ret = 1;
             } else {
                http_header[0] = 0;
             }
        }

closefile:
        fclose(fp);
    }

    return ret;
}



int init_client_header(char *fname)
{
	int header_len;
	if(!read_http_header(fname)) {
        return 0;
    }

	header_len = strlen(http_header);
	if ((http_header != NULL) && (header_len >= 16 )) {
		client_buf.base = http_header;
		client_buf.len = header_len;
		header_buf = &client_buf;
		return 1;
	} else {
		return 0;
	}
}

void init_server_header()
{
  ok_buf.base = "HTTP/1.1 200 OK\r\nConnection: Keep-Alive\r\n\r\nUpgrade";
  ok_buf.len = 50;

  fail_buf.base = "HTTP/1.1 301 Moved Permanently\r\nLocation: https://www.baidu.com/\r\n\r\n";
  fail_buf.len  =  68;
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
  sprintf(buf, "%s $'%s'", cmd, args);

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
    if (shell_run("./auth",  (char *)header)) {
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

