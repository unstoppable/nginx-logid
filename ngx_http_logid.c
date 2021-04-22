/* 
 *  Copyright (c) 2009 Mathieu Poumeyrol ( http://github.com/kali )
 *
 *  All rights reserved.
 *
 *  The following is released under the Creative Commons BSD license,
 *  available for your perusal at `http://creativecommons.org/licenses/BSD/`
 */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_md5.h>
#include <sys/stat.h>

typedef struct {
    u_char*   logid;
} ngx_http_logid_ctx_t;

typedef struct {
    ngx_flag_t  enable;
    ngx_str_t   header_name;
} ngx_http_logid_loc_conf_t;

static ngx_int_t ngx_http_logid_add_variables(ngx_conf_t *cf);
static void * ngx_http_logid_create_loc_conf(ngx_conf_t *cf);
static char * ngx_http_logid_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t ngx_http_logid_init_worker(ngx_cycle_t *cycle);

static uint16_t  start_value;
static uint16_t  sequencer_v2 = 0x0;

static ngx_command_t  ngx_http_logid_commands[] = {
    { ngx_string( "logid" ),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("logid_header"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof( ngx_http_logid_loc_conf_t, header_name),
      NULL },

      ngx_null_command
};

static ngx_http_module_t  ngx_http_logid_module_ctx = {
    ngx_http_logid_add_variables,    /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                   /* create main configuration */
    NULL,                                   /* init main configuration */

    NULL,                                   /* create server configuration */
    NULL,                                   /* merge server configuration */

    ngx_http_logid_create_loc_conf,   /* create location configuration */
    ngx_http_logid_merge_loc_conf,    /* merge location configuration */
};

ngx_module_t  ngx_http_logid_module = {
    NGX_MODULE_V1,
    &ngx_http_logid_module_ctx,   /* module context */
    ngx_http_logid_commands,      /* module directives */
    NGX_HTTP_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    ngx_http_logid_init_worker,   /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_str_t  ngx_http_logid_variable_name = ngx_string("logid");

static void * ngx_http_logid_create_loc_conf(ngx_conf_t *cf) {
    ngx_http_logid_loc_conf_t    *conf;

    conf = ngx_pcalloc( cf->pool, sizeof( ngx_http_logid_loc_conf_t ) );
    if ( NULL == conf ) {
        return NGX_CONF_ERROR;
    }
    conf->enable   = NGX_CONF_UNSET_UINT;
    return conf;
}

static char * ngx_http_logid_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_logid_loc_conf_t *prev = parent;
    ngx_http_logid_loc_conf_t *conf = child;

    ngx_conf_merge_value( conf->enable, prev->enable, 0 );
    ngx_conf_merge_str_value( conf->header_name, prev->header_name, "X-Ngx-LogId");

    return NGX_CONF_OK;
}

static u_char hex[] = "0123456789abcdef";

static ngx_http_logid_ctx_t *
ngx_http_logid_get_logid(ngx_http_request_t *r, ngx_http_logid_loc_conf_t *conf) {

    ngx_http_logid_ctx_t   *ctx;
    ngx_uint_t      found=0;
    ngx_uint_t      i;
    ngx_list_part_t *part = NULL;
    ngx_table_elt_t *header = NULL;

    ngx_connection_t     *c;
    struct sockaddr_in   *sin;
    uint16_t             rid[5];
    u_char               *rid_as_pc;
    uint32_t             tm;

    ctx = ngx_http_get_module_ctx(r, ngx_http_logid_module);
    if (ctx) {
        return ctx;
    }

    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_logid_ctx_t));
        if (ctx == NULL) {
            return NULL;
        }

        ngx_http_set_ctx(r, ctx, ngx_http_logid_module);
    }

    /* look for log id header in request */
    part = &r->headers_in.headers.part;
    header = part->elts;
    for ( i = 0 ; ; i++ ) {
        if ( i >= part->nelts) {
            if ( part->next == NULL ) {
                    break;
            }

            part = part->next;
            header = part->elts;
            i = 0;
        }

        if ( ngx_strcmp(header[i].key.data, conf->header_name.data) == 0 ) {
            ctx->logid = header[i].value.data;
            found = 1;
            break;
        }
    }

    if ( found ) {
       return ctx;
    }

    c = r->connection;

    if (ngx_connection_local_sockaddr(c, NULL, 0) != NGX_OK) {
        return NULL;
    }

    sin = (struct sockaddr_in *) c->local_sockaddr;
    tm = htonl((uint32_t)ngx_time());
    rid[0] = (uint16_t)(((uint32_t)sin->sin_addr.s_addr & 0x0000FFFF) + ((uint32_t)sin->sin_addr.s_addr>>16));
    rid[1] = (uint16_t)tm;
    rid[2] = (uint16_t)(tm>>16);
    rid[3] = (uint16_t)htons(start_value);
    rid[4] = (uint16_t)htons(sequencer_v2);

    sequencer_v2 ++;

    ctx->logid = (u_char*) ngx_pcalloc(r->pool, 21);
    ctx->logid[20] = 0;
    rid_as_pc = (u_char *) rid;
    for(i=0; i<10; i++) {
        ctx->logid[2*i]   = hex[rid_as_pc[i] >> 4];
        ctx->logid[2*i+1] = hex[rid_as_pc[i] & 0xf];
    }

    return ctx;
}

static ngx_int_t
ngx_http_logid_get_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_logid_ctx_t   *ctx;
    ngx_http_logid_loc_conf_t  *conf;

    conf = ngx_http_get_module_loc_conf(r->main, ngx_http_logid_module);
    if (!conf->enable) {
        v->not_found = 1;
        return NGX_OK;
    }
    ctx = ngx_http_logid_get_logid(r, conf);

    if (ctx == NULL) {
        return NGX_OK;
    }

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->len = ngx_strlen(ctx->logid);
    v->data = ctx->logid;

    return NGX_OK;
}


static ngx_int_t
ngx_http_logid_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var;

    var = ngx_http_add_variable(cf, &ngx_http_logid_variable_name, NGX_HTTP_VAR_NOHASH);
    if (var == NULL) {
        return NGX_ERROR;
    }

    var->get_handler = ngx_http_logid_get_variable;

    return NGX_OK;
}

static ngx_int_t
ngx_http_logid_init_worker(ngx_cycle_t *cycle)
{
    struct timeval  tp;

    ngx_gettimeofday(&tp);

    /* use the most significant usec part that fits to 16 bits */
    start_value = (uint16_t)ngx_pid;
    sequencer_v2 = (uint16_t)(tp.tv_usec / 20);

    return NGX_OK;
}
