
/*
 * Copyright (C) Yichun Zhang (agentzh)
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include "ngx_http_lua_timer.h"
#include "ngx_http_lua_util.h"
#include "ngx_http_lua_contentby.h"
#include "ngx_http_lua_probe.h"


#define NGX_HTTP_LUA_TIMER_ERRBUF_SIZE  128
#define ffz(x)                          __builtin_ctzl(~(x))

static const size_t BPL = sizeof(long) * 8; /* BITS_PER_LONG */

typedef struct {
    void        **main_conf;
    void        **srv_conf;
    void        **loc_conf;

    lua_State    *co;

    ngx_pool_t   *pool;

    ngx_listening_t                   *listening;
    ngx_str_t                          client_addr_text;

    ngx_http_lua_main_conf_t          *lmcf;
    ngx_http_lua_vm_state_t           *vm_state;

    int              co_ref;
    unsigned         delay:31;
    unsigned         premature:1;
    unsigned long    timer_ref;
} ngx_http_lua_timer_ctx_t;


struct ngx_lua_timer_reftable {
    ngx_event_t** ref;

    unsigned long *open_refs;
    unsigned long *full_refs_bits;

    /* max to ngx_int_t max_pending_timers; */
    unsigned long   max_refs; 
    unsigned long   next_ref;
};
static struct ngx_lua_timer_reftable *reftable = NULL;


static int ngx_http_lua_ngx_timer_at(lua_State *L);
static int ngx_http_lua_ngx_timer_every(lua_State *L);
static int ngx_http_lua_ngx_timer_cancel(lua_State *L);
static int ngx_http_lua_ngx_timer_helper(lua_State *L, int every);
static int ngx_http_lua_ngx_timer_running_count(lua_State *L);
static int ngx_http_lua_ngx_timer_pending_count(lua_State *L);
static ngx_int_t ngx_http_lua_timer_copy(ngx_http_lua_timer_ctx_t *old_tctx);
static void ngx_http_lua_timer_handler(ngx_event_t *ev);
static u_char *ngx_http_lua_log_timer_error(ngx_log_t *log, u_char *buf,
    size_t len);
static void ngx_http_lua_abort_pending_timers(ngx_event_t *ev);


static 
ngx_inline unsigned long bit_check(const unsigned long pos) {
    if (pos >= reftable->max_refs) return 1;
    return reftable->open_refs[pos / BPL] & (1UL << (pos % BPL));
}


static 
ngx_inline void bit_set(const unsigned long pos)
{
    if (pos >= reftable->max_refs) return;
    unsigned idx = pos / BPL;
    reftable->open_refs[pos / BPL] |= 1UL << (pos % BPL);
    if (reftable->open_refs[pos/BPL] == ~0UL) {
        reftable->full_refs_bits[idx/BPL] |= (1UL << (idx % BPL));
    }
}


static 
ngx_inline void bit_clean(const unsigned long pos)
{
    if (pos >= reftable->max_refs) return;
    unsigned idx = pos / BPL;
    reftable->open_refs[pos / BPL] &= ~(1UL << (pos % BPL));
    reftable->full_refs_bits[idx / BPL] &= ~(1UL << (idx % BPL));
}


static 
unsigned long find_first_zero_bit(const unsigned long start)
{
    if (!reftable) return ~0UL;
    if (!bit_check(start)) return start;
    unsigned long _start = start / BPL / BPL;
    unsigned long idx = ~0UL; // Marked as not found in ~0UL
    for (; _start * BPL * BPL < reftable->max_refs;
          _start++) {
        if (reftable->full_refs_bits[_start] != ~0UL) {
            idx = ffz(reftable->full_refs_bits[_start]);
            break;
        }
    }
    if (idx == ~0UL) return ~0UL;
    idx = ffz(reftable->open_refs[_start * BPL + idx]) + 
          (idx + _start * BPL) * BPL;

    if (idx >= reftable->max_refs) return ~0UL;
    return idx;
}


static
void expand_reftable(ngx_log_t *log) {
    if (!reftable) { /* init */
        reftable = ngx_alloc(sizeof(struct ngx_lua_timer_reftable), log);
        if (!reftable) {
            return;
        }
        reftable->next_ref = 0;
        reftable->max_refs = 0;
    }

    ngx_event_t   **new_ref           = NULL;
    unsigned long *new_open_refs      = NULL;
    unsigned long *new_full_refs_bits = NULL;

    unsigned long old_max_refs = reftable->max_refs;
    unsigned long max_refs = old_max_refs;

    if (!max_refs) { /* chose the next size to expand */
        max_refs = 1 << 10;
    } else if (max_refs < 64<<10) {
        max_refs *= 2;
    } else {
        max_refs += 10 << 10;
    }

    /* realloc start */

    /* alloc new max_refs */
    unsigned long total_size = 0;
    int size = max_refs / BPL + (max_refs % BPL == 0 ? 0 : 1);

    new_ref = ngx_alloc(sizeof(ngx_event_t *)*max_refs, log);
    if (!new_ref) {
        goto nomem;
    }
    total_size += sizeof(ngx_event_t *)*max_refs;


    /* alloc new open_refs */
    new_open_refs = ngx_alloc(size * sizeof(long), log);
    if (!new_open_refs) {
        goto nomem;
    }
    ngx_memzero(new_open_refs, size * sizeof(long));
    total_size += size * sizeof(long);

    /* alloc new full_refs_bits */
    size = size / BPL + (size % BPL == 0 ? 0 : 1);
    new_full_refs_bits = ngx_alloc(size * sizeof(long), log);
    if (!new_full_refs_bits) {
        goto nomem;
    }
    ngx_memzero(new_full_refs_bits, size * sizeof(long));
    total_size += size * sizeof(long);

    if (old_max_refs) { /* memcpy && free old */
        int old_size = old_max_refs / BPL + (old_max_refs % BPL == 0 ? 0 : 1);

        ngx_memcpy(new_ref, reftable->ref,
                   sizeof(ngx_event_t *) * old_max_refs);
        ngx_free(reftable->ref);

        ngx_memcpy(new_open_refs, reftable->open_refs,
                   sizeof(long) * old_size);
        ngx_free(reftable->open_refs);
        old_size = old_size / BPL + (old_size % BPL == 0 ? 0 : 1);

        ngx_memcpy(new_full_refs_bits, reftable->full_refs_bits, 
                   sizeof(long) * old_size);
        ngx_free(reftable->full_refs_bits);
    }

    reftable->max_refs = max_refs;

    reftable->ref = new_ref;

    reftable->open_refs = new_open_refs;

    reftable->full_refs_bits = new_full_refs_bits;

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                       "ngx.timer.reftable expand from %ul to %ul (%ul byte)",
                        old_max_refs, max_refs, total_size);
    /* finish */
    return;
nomem:
    if (new_ref) {
        ngx_free(new_ref);
    }
    if (new_open_refs) {
        ngx_free(new_open_refs);
    }
    if (new_full_refs_bits) {
        ngx_free(new_full_refs_bits);
    }
}


void
ngx_http_lua_inject_timer_api(ngx_log_t *log, lua_State *L)
{
    lua_createtable(L, 0 /* narr */, 5 /* nrec */);    /* ngx.timer. */

    lua_pushcfunction(L, ngx_http_lua_ngx_timer_at);
    lua_setfield(L, -2, "at");

    lua_pushcfunction(L, ngx_http_lua_ngx_timer_every);
    lua_setfield(L, -2, "every");

    lua_pushcfunction(L, ngx_http_lua_ngx_timer_cancel);
    lua_setfield(L, -2, "cancel");

    lua_pushcfunction(L, ngx_http_lua_ngx_timer_running_count);
    lua_setfield(L, -2, "running_count");

    lua_pushcfunction(L, ngx_http_lua_ngx_timer_pending_count);
    lua_setfield(L, -2, "pending_count");

    lua_setfield(L, -2, "timer");

    /* first expand to init */
    expand_reftable(log);
}


static int
ngx_http_lua_ngx_timer_running_count(lua_State *L)
{
    ngx_http_request_t          *r;
    ngx_http_lua_main_conf_t    *lmcf;

    r = ngx_http_lua_get_req(L);
    if (r == NULL) {
        return luaL_error(L, "no request");
    }

    lmcf = ngx_http_get_module_main_conf(r, ngx_http_lua_module);

    lua_pushnumber(L, lmcf->running_timers);

    return 1;
}


static int
ngx_http_lua_ngx_timer_pending_count(lua_State *L)
{
    ngx_http_request_t          *r;
    ngx_http_lua_main_conf_t    *lmcf;

    r = ngx_http_lua_get_req(L);
    if (r == NULL) {
        return luaL_error(L, "no request");
    }

    lmcf = ngx_http_get_module_main_conf(r, ngx_http_lua_module);

    lua_pushnumber(L, lmcf->pending_timers);

    return 1;
}


static int
ngx_http_lua_ngx_timer_at(lua_State *L)
{
    return ngx_http_lua_ngx_timer_helper(L, 0);
}


/*
 * TODO: return a timer handler instead which can be passed to
 * the ngx.timer.cancel method to cancel the timer.
 */
static int
ngx_http_lua_ngx_timer_every(lua_State *L)
{
    return ngx_http_lua_ngx_timer_helper(L, 1);
}


static int 
ngx_http_lua_ngx_timer_cancel(lua_State *L) 
{
    int                             nargs;
    ngx_event_t                     *ev;
    ngx_http_request_t              *r;
    ngx_http_lua_main_conf_t        *lmcf;
    ngx_http_lua_timer_ctx_t        tctx;

    nargs = lua_gettop(L);

    if (nargs < 1) {
        return luaL_error(L, "expecting at least 1 arguments but got %d",
                          nargs);
    }

    unsigned long ref = (unsigned long)luaL_checknumber(L, 1);

    if (!reftable || ref >= reftable->max_refs || !bit_check(ref))  {
        lua_pushnil(L);
        lua_pushliteral(L, "timer does not exist");
        return 2;
    }
    
    r = ngx_http_lua_get_req(L);
    if (r == NULL) {
        return luaL_error(L, "no request");
    }
    
    ev = reftable->ref[ref];

    ngx_memcpy(&tctx, ev->data, sizeof(ngx_http_lua_timer_ctx_t));
    
    if (tctx.delay) {
        ngx_del_timer(ev);
    } else {

#ifdef HAVE_POSTED_DELAYED_EVENTS_PATCH
        ngx_delete_posted_event(ev);
#else
        ngx_del_timer(ev);
#endif

    }
    
    ngx_free(ev);

    ngx_http_lua_assert(tctx.co_ref && tctx.co);

    lmcf = tctx.lmcf;
    lmcf->pending_timers--;


    ngx_http_lua_free_thread(r, L, tctx.co_ref, tctx.co, lmcf);

    if (tctx.vm_state != NULL) {
        ngx_http_lua_cleanup_vm(tctx.vm_state);
    }

    if (tctx.pool) {
        ngx_destroy_pool(tctx.pool);
    }

    bit_clean(ref);

    if (ref < reftable->next_ref) reftable->next_ref = ref;

    lua_pushinteger(L, 1);
    return 1;
}


static int
ngx_http_lua_ngx_timer_helper(lua_State *L, int every)
{
    int                      nargs;
    int                      co_ref;
    u_char                  *p;
    lua_State               *vm;  /* the main thread */
    lua_State               *co;
    ngx_msec_t               delay;
    ngx_event_t             *ev = NULL;
    ngx_http_request_t      *r;
    ngx_connection_t        *saved_c = NULL;
    ngx_http_lua_ctx_t      *ctx;
#if 0
    ngx_http_connection_t   *hc;
#endif

    ngx_http_lua_timer_ctx_t      *tctx = NULL;
    ngx_http_lua_main_conf_t      *lmcf;
#if 0
    ngx_http_core_main_conf_t     *cmcf;
#endif

    nargs = lua_gettop(L);
    if (nargs < 2) {
        return luaL_error(L, "expecting at least 2 arguments but got %d",
                          nargs);
    }

    delay = (ngx_msec_t) (luaL_checknumber(L, 1) * 1000);

    if (every && delay == 0) {
        return luaL_error(L, "delay cannot be zero");
    }

    luaL_argcheck(L, lua_isfunction(L, 2) && !lua_iscfunction(L, 2), 2,
                  "Lua function expected");

    r = ngx_http_lua_get_req(L);
    if (r == NULL) {
        return luaL_error(L, "no request");
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);

    if (ngx_exiting && delay > 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "process exiting");
        return 2;
    }

    lmcf = ngx_http_get_module_main_conf(r, ngx_http_lua_module);

    if (lmcf->pending_timers >= lmcf->max_pending_timers) {
        lua_pushnil(L);
        lua_pushliteral(L, "too many pending timers");
        return 2;
    }

    if (lmcf->watcher == NULL) {
        /* create the watcher fake connection */

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                       "lua creating fake watcher connection");

        if (ngx_cycle->files) {
            saved_c = ngx_cycle->files[0];
        }

        lmcf->watcher = ngx_get_connection(0, ngx_cycle->log);

        if (ngx_cycle->files) {
            ngx_cycle->files[0] = saved_c;
        }

        if (lmcf->watcher == NULL) {
            return luaL_error(L, "no memory");
        }

        /* to work around the -1 check in ngx_worker_process_cycle: */
        lmcf->watcher->fd = (ngx_socket_t) -2;

        lmcf->watcher->idle = 1;
        lmcf->watcher->read->handler = ngx_http_lua_abort_pending_timers;
        lmcf->watcher->data = lmcf;
    }

    vm = ngx_http_lua_get_lua_vm(r, ctx);

    co_ref = ngx_http_lua_new_cached_thread(vm, &co, lmcf, 1);

    /* vm stack: coroutines thread */

    /* L stack: time func [args] */

    ngx_http_lua_probe_user_coroutine_create(r, L, co);

    /* co stack: <empty> */

    dd("stack top: %d", lua_gettop(L));

    lua_xmove(vm, L, 1);    /* move coroutine from main thread to L */

    lua_pop(vm, 1);  /* pop coroutines */

    /* L stack: time func [args] thread */
    /* vm stack: empty */

    lua_pushvalue(L, 2);    /* copy entry function to top of L*/

    /* L stack: time func [args] thread func */

    lua_xmove(L, co, 1);    /* move entry function from L to co */

    /* L stack: time func [args] thread */
    /* co stack: func */

#ifndef OPENRESTY_LUAJIT
    ngx_http_lua_get_globals_table(co);
    lua_setfenv(co, -2);
#endif

    /* co stack: func */

    /* L stack: time func [args] thread */

    if (nargs > 2) {
        lua_pop(L, 1);  /* L stack: time func [args] */
        lua_xmove(L, co, nargs - 2);  /* L stack: time func */

        /* co stack: func [args] */
    }

    p = ngx_alloc(sizeof(ngx_event_t) + sizeof(ngx_http_lua_timer_ctx_t),
                  r->connection->log);
    if (p == NULL) {
        goto nomem;
    }

    ev = (ngx_event_t *) p;

    ngx_memzero(ev, sizeof(ngx_event_t));

    p += sizeof(ngx_event_t);

    tctx = (ngx_http_lua_timer_ctx_t *) p;

    tctx->delay = every ? delay : 0;

    tctx->premature = 0;
    tctx->co_ref = co_ref;
    tctx->co = co;
    tctx->main_conf = r->main_conf;
    tctx->srv_conf = r->srv_conf;
    tctx->loc_conf = r->loc_conf;
    tctx->lmcf = lmcf;

    tctx->pool = ngx_create_pool(128, ngx_cycle->log);
    if (tctx->pool == NULL) {
        goto nomem;
    }

    if (r->connection) {
        tctx->listening = r->connection->listening;

    } else {
        tctx->listening = NULL;
    }

    if (r->connection->addr_text.len) {
        tctx->client_addr_text.data = ngx_palloc(tctx->pool,
                                                 r->connection->addr_text.len);
        if (tctx->client_addr_text.data == NULL) {
            goto nomem;
        }

        ngx_memcpy(tctx->client_addr_text.data, r->connection->addr_text.data,
                   r->connection->addr_text.len);
        tctx->client_addr_text.len = r->connection->addr_text.len;

    } else {
        tctx->client_addr_text.len = 0;
        tctx->client_addr_text.data = NULL;
    }

    if (ctx && ctx->vm_state) {
        tctx->vm_state = ctx->vm_state;
        tctx->vm_state->count++;

    } else {
        tctx->vm_state = NULL;
    }

    ev->handler = ngx_http_lua_timer_handler;
    ev->data = tctx;
    ev->log = ngx_cycle->log;

    unsigned long ref = find_first_zero_bit(reftable->next_ref);

    if (ref == ~0UL) {
        /* try to expand reftable */
        expand_reftable(ngx_cycle->log);
        ref = find_first_zero_bit(reftable->next_ref);
    }
    
    if (ref == ~0UL) { /* double check */
        ngx_log_debug2(NGX_LOG_ERR, ngx_cycle->log, 0,
                   "expand reftable error, next_ref:%ud, max_refs:%ud", 
                   reftable ? reftable->next_ref : 0, reftable ? reftable->max_refs : 0);

        if (tctx && tctx->pool) {
            ngx_destroy_pool(tctx->pool);
        }

        if (tctx && tctx->vm_state) {
            tctx->vm_state->count--;
        }

        ngx_http_lua_free_thread(r, L, co_ref, co, lmcf);
        
        ngx_free(ev);

        return luaL_error(L, "ngx.timer reftable expand error");
    }

    lmcf->pending_timers++;

    bit_set(ref);
    reftable->next_ref = ref + 1;
    reftable->ref[ref] = ev;
    tctx->timer_ref = ref;

#ifdef HAVE_POSTED_DELAYED_EVENTS_PATCH
    if (delay == 0 && !ngx_exiting) {
        dd("posting 0 sec sleep event to head of delayed queue");
        ngx_post_event(ev, &ngx_posted_delayed_events);

        lua_pushinteger(L, tctx->timer_ref);
        return 1;
    }
#endif

    ngx_add_timer(ev, delay);

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "created timer (co: %p delay: %M ms): sz=%d", tctx->co,
                   delay, lua_gettop(L));

    lua_pushinteger(L, tctx->timer_ref);
    return 1;

nomem:

    if (tctx && tctx->pool) {
        ngx_destroy_pool(tctx->pool);
    }

    if (ev) {
        ngx_free(ev);
    }

    ngx_http_lua_free_thread(r, L, co_ref, co, lmcf);

    return luaL_error(L, "no memory");
}


static ngx_int_t
ngx_http_lua_timer_copy(ngx_http_lua_timer_ctx_t *old_tctx)
{
    int                          nargs, co_ref, i;
    u_char                      *p;
    lua_State                   *vm;  /* the main thread */
    lua_State                   *co;
    lua_State                   *L;
    ngx_event_t                 *ev = NULL;
    ngx_http_lua_timer_ctx_t    *tctx = NULL;
    ngx_http_lua_main_conf_t    *lmcf;

    /* L stack: func [args] */
    L = old_tctx->co;

    lmcf = old_tctx->lmcf;

    vm = old_tctx->vm_state ? old_tctx->vm_state->vm : lmcf->lua;

    co_ref = ngx_http_lua_new_cached_thread(vm, &co, lmcf, 1);

    /* co stack: <empty> */

    dd("stack top: %d", lua_gettop(L));

    lua_xmove(vm, L, 1);    /* move coroutine from main thread to L */

    lua_pop(vm, 1);  /* pop coroutines */

    /* L stack: func [args] thread */
    /* vm stack: empty */

    lua_pushvalue(L, 1);    /* copy entry function to top of L*/

    /* L stack: func [args] thread func */

    lua_xmove(L, co, 1);    /* move entry function from L to co */

    /* L stack: func [args] thread */
    /* co stack: func */

#ifndef OPENRESTY_LUAJIT
    ngx_http_lua_get_globals_table(co);
    lua_setfenv(co, -2);
#endif

    /* co stack: func */

    lua_pop(L, 1);  /* pop thread */

    /* L stack: func [args] */

    nargs = lua_gettop(L);
    if (nargs > 1) {
        for (i = 2; i <= nargs; i++) {
            lua_pushvalue(L, i);
        }

        /* L stack: func [args] [args] */

        lua_xmove(L, co, nargs - 1);

        /* L stack: func [args] */
        /* co stack: func [args] */
    }

    p = ngx_alloc(sizeof(ngx_event_t) + sizeof(ngx_http_lua_timer_ctx_t),
                  ngx_cycle->log);
    if (p == NULL) {
        goto nomem;
    }

    ev = (ngx_event_t *) p;

    ngx_memzero(ev, sizeof(ngx_event_t));

    p += sizeof(ngx_event_t);

    tctx = (ngx_http_lua_timer_ctx_t *) p;

    ngx_memcpy(tctx, old_tctx, sizeof(ngx_http_lua_timer_ctx_t));

    tctx->co_ref = co_ref;
    tctx->co = co;

    tctx->pool = ngx_create_pool(128, ngx_cycle->log);
    if (tctx->pool == NULL) {
        goto nomem;
    }

    if (tctx->client_addr_text.len) {
        tctx->client_addr_text.data = ngx_palloc(tctx->pool,
                                                 tctx->client_addr_text.len);
        if (tctx->client_addr_text.data == NULL) {
            goto nomem;
        }

        ngx_memcpy(tctx->client_addr_text.data, old_tctx->client_addr_text.data,
                   tctx->client_addr_text.len);
    }

    if (tctx->vm_state) {
        tctx->vm_state->count++;
    }

    ev->handler = ngx_http_lua_timer_handler;
    ev->data = tctx;
    ev->log = ngx_cycle->log;

    lmcf->pending_timers++;

    ngx_add_timer(ev, tctx->delay);
    
    /* reset with the new timer (ngx_event_t*) */
    reftable->ref[tctx->timer_ref] = ev;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "created next timer (co: %p delay: %M ms)", tctx->co,
                   tctx->delay);

    return NGX_OK;

nomem:

    if (tctx && tctx->pool) {
        ngx_destroy_pool(tctx->pool);
    }

    if (ev) {
        ngx_free(ev);
    }

    /* L stack: func [args] */

    ngx_http_lua_free_thread(NULL, L, co_ref, co, lmcf);

    return NGX_ERROR;
}


static void
ngx_http_lua_timer_handler(ngx_event_t *ev)
{
    int                      n;
    lua_State               *L = NULL;
    ngx_int_t                rc;
    ngx_connection_t        *c = NULL;
    ngx_http_request_t      *r = NULL;
    ngx_http_lua_ctx_t      *ctx;
    ngx_http_cleanup_t      *cln;
    ngx_pool_cleanup_t      *pcln;

    ngx_http_lua_timer_ctx_t         tctx;
    ngx_http_lua_main_conf_t        *lmcf;
    ngx_http_core_loc_conf_t        *clcf;

    lua_Debug                ar;
    u_char                  *p;
    u_char                   errbuf[NGX_HTTP_LUA_TIMER_ERRBUF_SIZE];
    const char              *source;
    const char              *errmsg;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "lua ngx.timer expired");

    ngx_memcpy(&tctx, ev->data, sizeof(ngx_http_lua_timer_ctx_t));
    ngx_free(ev);

    ngx_http_lua_assert(tctx.co_ref && tctx.co);

    lmcf = tctx.lmcf;

    lmcf->pending_timers--;

    if (!ngx_exiting && tctx.delay > 0) {
        rc = ngx_http_lua_timer_copy(&tctx);
        if (rc != NGX_OK) {

            bit_clean(tctx.timer_ref);
            if (tctx.timer_ref < reftable->next_ref) {
                reftable->next_ref = tctx.timer_ref;
            }

            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                          "failed to create the next timer of delay %ud ms",
                          (unsigned) tctx.delay);
        }
    } else {
        bit_clean(tctx.timer_ref);
        if (tctx.timer_ref < reftable->next_ref) {
            reftable->next_ref = tctx.timer_ref;
        }
    }

    if (lmcf->running_timers >= lmcf->max_running_timers) {
        p = ngx_snprintf(errbuf, NGX_HTTP_LUA_TIMER_ERRBUF_SIZE - 1,
                         "%i lua_max_running_timers are not enough",
                         lmcf->max_running_timers);
        *p = '\0';
        errmsg = (const char *) errbuf;
        goto failed;
    }

    c = ngx_http_lua_create_fake_connection(tctx.pool);
    if (c == NULL) {
        errmsg = "could not create fake connection";
        goto failed;
    }

    c->log->handler = ngx_http_lua_log_timer_error;
    c->log->data = c;

    c->listening = tctx.listening;
    c->addr_text = tctx.client_addr_text;

    r = ngx_http_lua_create_fake_request(c);
    if (r == NULL) {
        errmsg = "could not create fake request";
        goto failed;
    }

    r->main_conf = tctx.main_conf;
    r->srv_conf = tctx.srv_conf;
    r->loc_conf = tctx.loc_conf;

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

#if (nginx_version >= 1009000)
    ngx_set_connection_log(r->connection, clcf->error_log);

#else
    ngx_http_set_connection_log(r->connection, clcf->error_log);
#endif

    dd("lmcf: %p", lmcf);

    ctx = ngx_http_lua_create_ctx(r);
    if (ctx == NULL) {
        errmsg = "could not create ctx";
        goto failed;
    }

    if (tctx.vm_state) {
        ctx->vm_state = tctx.vm_state;

        pcln = ngx_pool_cleanup_add(r->pool, 0);
        if (pcln == NULL) {
            errmsg = "could not add vm cleanup";
            goto failed;
        }

        pcln->handler = ngx_http_lua_cleanup_vm;
        pcln->data = tctx.vm_state;
    }

    ctx->cur_co_ctx = &ctx->entry_co_ctx;

    L = ngx_http_lua_get_lua_vm(r, ctx);

    cln = ngx_http_cleanup_add(r, 0);
    if (cln == NULL) {
        errmsg = "could not add request cleanup";
        goto failed;
    }

    cln->handler = ngx_http_lua_request_cleanup_handler;
    cln->data = ctx;
    ctx->cleanup = &cln->handler;

    ctx->entered_content_phase = 1;
    ctx->context = NGX_HTTP_LUA_CONTEXT_TIMER;

    r->read_event_handler = ngx_http_block_reading;

    ctx->cur_co_ctx->co_ref = tctx.co_ref;
    ctx->cur_co_ctx->co = tctx.co;
    ctx->cur_co_ctx->co_status = NGX_HTTP_LUA_CO_RUNNING;

    dd("r connection: %p, log %p", r->connection, r->connection->log);

    /*  save the request in coroutine globals table */
    ngx_http_lua_set_req(tctx.co, r);

    ngx_http_lua_attach_co_ctx_to_L(tctx.co, ctx->cur_co_ctx);

    lmcf->running_timers++;

    lua_pushboolean(tctx.co, tctx.premature);

    n = lua_gettop(tctx.co);
    if (n > 2) {
        lua_insert(tctx.co, 2);
    }

#ifdef NGX_LUA_USE_ASSERT
    ctx->cur_co_ctx->co_top = 1;
#endif

    rc = ngx_http_lua_run_thread(L, r, ctx, n - 1);

    dd("timer lua run thread: %d", (int) rc);

    if (rc == NGX_ERROR || rc >= NGX_OK) {
        /* do nothing */

    } else if (rc == NGX_AGAIN) {
        rc = ngx_http_lua_content_run_posted_threads(L, r, ctx, 0);

    } else if (rc == NGX_DONE) {
        rc = ngx_http_lua_content_run_posted_threads(L, r, ctx, 1);

    } else {
        rc = NGX_OK;
    }

    ngx_http_lua_finalize_request(r, rc);
    return;

failed:

    /* co stack: func [args] */
    lua_pushvalue(tctx.co, 1);
    /* co stack: func [args] func */
    lua_getinfo(tctx.co, ">Sf", &ar);

    source = ar.source;

    if (source == NULL) {
        source = "(unknown)";
    }

    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                  "lua failed to run timer with function defined at %s:%d: %s",
                  source, ar.linedefined, errmsg);

#if 1
    if (L == NULL) {
        if (tctx.vm_state != NULL) {
            L = tctx.vm_state->vm;
        }

        if (L == NULL) {
            L = lmcf->lua;
        }
    }
#endif

    if (L != NULL) {
        ngx_http_lua_free_thread(r, L, tctx.co_ref, tctx.co, lmcf);
    }

    if (tctx.vm_state != NULL) {
        ngx_http_lua_cleanup_vm(tctx.vm_state);
    }

    if (c != NULL) {
        ngx_http_lua_close_fake_connection(c);

    } else if (tctx.pool) {
        ngx_destroy_pool(tctx.pool);
    }
}


static u_char *
ngx_http_lua_log_timer_error(ngx_log_t *log, u_char *buf, size_t len)
{
    u_char              *p;
    ngx_connection_t    *c;

    if (log->action) {
        p = ngx_snprintf(buf, len, " while %s", log->action);
        len -= p - buf;
        buf = p;
    }

    c = log->data;

    dd("ctx = %p", c);

    p = ngx_snprintf(buf, len, ", context: ngx.timer");
    len -= p - buf;
    buf = p;

    if (c != NULL) {
        if (c->addr_text.len) {
            p = ngx_snprintf(buf, len, ", client: %V", &c->addr_text);
            len -= p - buf;
            buf = p;
        }

        if (c->listening && c->listening->addr_text.len) {
            p = ngx_snprintf(buf, len, ", server: %V",
                             &c->listening->addr_text);
            /* len -= p - buf; */
            buf = p;
        }
    }

    return buf;
}


static void
ngx_http_lua_abort_pending_timers(ngx_event_t *ev)
{
    ngx_int_t                    i, n;
    ngx_event_t                **events;
    ngx_connection_t            *c, *saved_c = NULL;
    ngx_rbtree_node_t           *cur, *prev, *next, *sentinel, *temp;
    ngx_http_lua_timer_ctx_t    *tctx;
    ngx_http_lua_main_conf_t    *lmcf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "lua abort pending timers");

    c = ev->data;
    lmcf = c->data;

    dd("lua connection fd: %d", (int) c->fd);

    if (!c->close) {
        return;
    }

    c->read->closed = 1;
    c->write->closed = 1;

    /* we temporarily use a valid fd (0) to make ngx_free_connection happy */

    c->fd = 0;

    if (ngx_cycle->files) {
        saved_c = ngx_cycle->files[0];
    }

    ngx_free_connection(c);

    c->fd = (ngx_socket_t) -1;

    if (ngx_cycle->files) {
        ngx_cycle->files[0] = saved_c;
    }

    if (lmcf->pending_timers == 0) {
        return;
    }

    /* expire pending timers immediately */

    sentinel = ngx_event_timer_rbtree.sentinel;

    cur = ngx_event_timer_rbtree.root;

    /* XXX nginx does not guarantee the parent of root is meaningful,
     * so we temporarily override it to simplify tree traversal. */
    temp = cur->parent;
    cur->parent = NULL;

    prev = NULL;

    events = ngx_pcalloc(ngx_cycle->pool,
                         lmcf->pending_timers * sizeof(ngx_event_t));
    if (events == NULL) {
        return;
    }

    n = 0;

    dd("root: %p, root parent: %p, sentinel: %p", cur, cur->parent, sentinel);

    while (n < lmcf->pending_timers) {
        if  (cur == sentinel || cur == NULL) {
            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                          "lua pending timer counter got out of sync: %i",
                          lmcf->pending_timers);
            break;
        }

        dd("prev: %p, cur: %p, cur parent: %p, cur left: %p, cur right: %p",
           prev, cur, cur->parent, cur->left, cur->right);

        if (prev == cur->parent) {
            /* neither of the children has been accessed yet */

            next = cur->left;
            if (next == sentinel) {
                ev = (ngx_event_t *)
                    ((char *) cur - offsetof(ngx_event_t, timer));

                if (ev->handler == ngx_http_lua_timer_handler) {
                    dd("found node: %p", cur);
                    events[n++] = ev;
                }

                next = (cur->right != sentinel) ? cur->right : cur->parent;
            }

        } else if (prev == cur->left) {
            /* just accessed the left child */

            ev = (ngx_event_t *)
                ((char *) cur - offsetof(ngx_event_t, timer));

            if (ev->handler == ngx_http_lua_timer_handler) {
                dd("found node 2: %p", cur);
                events[n++] = ev;
            }

            next = (cur->right != sentinel) ? cur->right : cur->parent;

        } else if (prev == cur->right) {
            /* already accessed both children */
            next = cur->parent;

        } else {
            /* not reacheable */
            next = NULL;
        }

        prev = cur;
        cur = next;
    }

    /* restore the old tree root's parent */
    ngx_event_timer_rbtree.root->parent = temp;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "lua found %i pending timers to be aborted prematurely",
                   n);

    for (i = 0; i < n; i++) {
        ev = events[i];

        ngx_rbtree_delete(&ngx_event_timer_rbtree, &ev->timer);

#if (NGX_DEBUG)
        ev->timer.left = NULL;
        ev->timer.right = NULL;
        ev->timer.parent = NULL;
#endif

        ev->timer_set = 0;

        ev->timedout = 1;

        tctx = ev->data;
        tctx->premature = 1;

        dd("calling timer handler prematurely");
        ev->handler(ev);
    }

#if 0
    if (pending_timers) {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                      "lua pending timer counter got out of sync: %i",
                      pending_timers);
    }
#endif
}

/* vi:set ft=c ts=4 sw=4 et fdm=marker: */
