/*
 * nginx module to log the full request content
 *
 * mattr@bit.ly 2010-12-15
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

typedef struct {
    ngx_open_file_t                 *file;
    time_t                          disk_full_time;
    time_t                          error_log_time;
} ngx_http_full_request_log_t;

typedef struct {
    ngx_flag_t                      enable;
} ngx_http_full_request_log_main_conf_t;

typedef struct {
    ngx_http_full_request_log_t     *log;
    ngx_flag_t                      off;
} ngx_http_full_request_log_loc_conf_t;

static ngx_int_t ngx_http_full_request_log_handler(ngx_http_request_t *r);
static void ngx_http_full_request_log_body_handler(ngx_http_request_t *r);
static void ngx_http_full_request_log_write(ngx_http_request_t *r, ngx_http_full_request_log_t *log, u_char *buf, size_t len);
static void *ngx_http_full_request_log_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_full_request_log_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_full_request_log_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static char *ngx_http_full_request_log_set_log(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_full_request_log_init(ngx_conf_t *cf);

static ngx_command_t ngx_http_full_request_log_commands[] = {
    { ngx_string("full_request_log_enable"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },
    { ngx_string("full_request_log"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_TAKE1,
      ngx_http_full_request_log_set_log,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
    ngx_null_command
};

static ngx_http_module_t  ngx_http_full_request_log_module_ctx = {
    NULL,                                               /* preconfiguration */
    ngx_http_full_request_log_init,                     /* postconfiguration */

    ngx_http_full_request_log_create_main_conf,         /* create main configuration */
    NULL,                                               /* init main configuration */

    NULL,                                               /* create server configuration */
    NULL,                                               /* merge server configuration */

    ngx_http_full_request_log_create_loc_conf,          /* create location configuration */
    ngx_http_full_request_log_merge_loc_conf            /* merge location configuration */
};

ngx_module_t ngx_http_full_request_log_module = {
    NGX_MODULE_V1,
    &ngx_http_full_request_log_module_ctx,      /* module context */
    ngx_http_full_request_log_commands,         /* module directives */
    NGX_HTTP_MODULE,                            /* module type */
    NULL,                                       /* init master */
    NULL,                                       /* init module */
    NULL,                                       /* init process */
    NULL,                                       /* init thread */
    NULL,                                       /* exit thread */
    NULL,                                       /* exit process */
    NULL,                                       /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t ngx_http_full_request_log_handler(ngx_http_request_t *r)
{
    ngx_int_t                               rc;
    ngx_http_full_request_log_loc_conf_t    *lcf;
    ngx_http_full_request_log_t             *log;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "http full request log handler");

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_full_request_log_module);

    if (lcf->off) {
        return NGX_OK;
    }

    log = lcf->log;
    if (ngx_time() == log->disk_full_time) {
        /*
         * on FreeBSD writing to a full filesystem with enabled softupdates
         * may block process for much longer time than writing to non-full
         * filesystem, so we skip writing to a log for one second
         */
        return NGX_OK;
    }

    rc = ngx_http_read_client_request_body(r, ngx_http_full_request_log_body_handler);

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return NGX_DONE;
}

static void ngx_http_full_request_log_body_handler(ngx_http_request_t *r)
{
    ngx_http_full_request_log_loc_conf_t    *lcf;
    ngx_http_full_request_log_t             *log;
    size_t                                  len;
    ngx_buf_t                               *b, *buf, *next;
    ngx_uint_t                              i;
    ngx_list_part_t                         *part;
    ngx_table_elt_t                         *header;
    ngx_chain_t                             *cl;

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_full_request_log_module);
    log = lcf->log;

    len = 50 + sizeof(LF) - 1 + r->request_line.len + sizeof(LF) - 1;
    part = &r->headers_in.headers.part;
    header = part->elts;
    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            header = part->elts;
            i = 0;
        }
        len += sizeof('> ') - 1 + header[i].key.len + sizeof(": ") - 1 + header[i].value.len + sizeof(LF) - 1;
    }

    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        return;
    }

    b->last = ngx_copy(b->last, "==================================================", 50);
    *b->last++ = LF;

    b->last = ngx_copy(b->last, r->request_line.data, r->request_line.len);
    *b->last++ = LF;

    part = &r->headers_in.headers.part;
    header = part->elts;
    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            header = part->elts;
            i = 0;
        }

        *b->last++ = '>';
        *b->last++ = ' ';
        b->last = ngx_copy(b->last, header[i].key.data, header[i].key.len);
        *b->last++ = ':';
        *b->last++ = ' ';
        b->last = ngx_copy(b->last, header[i].value.data, header[i].value.len);
        *b->last++ = LF;
    }

    *b->last++ = LF;
    ngx_http_full_request_log_write(r, log, b->pos, len);

    if (r->request_body == NULL || r->request_body->bufs == NULL) {
        return;
    }

    cl = r->request_body->bufs;
    buf = cl->buf;

    len = buf->last - buf->pos;
    ngx_http_full_request_log_write(r, log, buf->pos, len);

    if (cl->next != NULL) {
        next = cl->next->buf;
        len = next->last - next->pos;
        ngx_http_full_request_log_write(r, log, next->pos, len);
    }

    ngx_http_full_request_log_write(r, log, (unsigned char *)"\r\n", 2);
}

static void ngx_http_full_request_log_write(ngx_http_request_t *r, ngx_http_full_request_log_t *log, u_char *buf, size_t len)
{
    u_char     *name;
    time_t      now;
    ssize_t     n;
    ngx_err_t   err;

    name = log->file->name.data;
    n = ngx_write_fd(log->file->fd, buf, len);

    if (n == (ssize_t)len) {
        return;
    }

    now = ngx_time();
    if (n == -1) {
        err = ngx_errno;

        if (err == NGX_ENOSPC) {
            log->disk_full_time = now;
        }

        if (now - log->error_log_time > 59) {
            ngx_log_error(NGX_LOG_ALERT, r->connection->log, err, ngx_write_fd_n " to \"%s\" failed", name);
            log->error_log_time = now;
        }

        return;
    }

    if (now - log->error_log_time > 59) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0, ngx_write_fd_n " to \"%s\" was incomplete: %z of %uz", name, n, len);
        log->error_log_time = now;
    }
}

static ngx_int_t ngx_http_full_request_log_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt                     *h;
    ngx_http_full_request_log_main_conf_t   *lmcf;
    ngx_http_core_main_conf_t               *cmcf;

    lmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_full_request_log_module);
    if (lmcf->enable) {
        cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

        h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
        if (h == NULL) {
            return NGX_ERROR;
        }

        *h = ngx_http_full_request_log_handler;
    }

    return NGX_OK;
}

static char *ngx_http_full_request_log_set_log(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                               *value;
    ngx_http_full_request_log_main_conf_t   *lmcf;
    ngx_http_full_request_log_loc_conf_t    *llcf = conf;

    lmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_full_request_log_module);
    if (lmcf->enable) {
        value = cf->args->elts;

        if (ngx_strcmp(value[1].data, "off") == 0) {
            llcf->off = 1;
            return NGX_CONF_OK;
        }

        llcf->off = 0;
        llcf->log = ngx_pcalloc(cf->pool, sizeof(ngx_http_full_request_log_t));
        ngx_memzero(llcf->log, sizeof(ngx_http_full_request_log_t));
        llcf->log->file = ngx_conf_open_file(cf->cycle, &value[1]);
        if (llcf->log->file == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}

static void *ngx_http_full_request_log_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_full_request_log_main_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_full_request_log_main_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->enable = NGX_CONF_UNSET;

    return conf;
}

static void *ngx_http_full_request_log_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_full_request_log_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_full_request_log_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->off = NGX_CONF_UNSET;

    return conf;
}

static char *ngx_http_full_request_log_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_full_request_log_loc_conf_t *prev = parent;
    ngx_http_full_request_log_loc_conf_t *conf = child;

    if (conf->log || (conf->off != NGX_CONF_UNSET)) {
        return NGX_CONF_OK;
    }

    conf->log = prev->log;
    conf->off = prev->off;

    return NGX_CONF_OK;
}
