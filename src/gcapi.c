/*
 *
 * GrizzlyCloud library - simplified VPN alternative for IoT
 * Copyright (C) 2016 - 2017 Filip Pancik
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <gc.h>

//static ev_timer connect_timer;
static struct gc_s *gclocal = NULL;
int gc_sigterm = 0;

int gc_message_from(struct gc_s *gc, struct proto_s *p)
{
    char **argv;
    int argc;
    int ret;

    ret = gc_parse_header_mf(p->u.message_from.tp, &argv, &argc);
    if(ret != GC_OK) {
        if(argv) free(argv);
        return ret;
    }

    if(argc < 1) {
        if(argv) free(argv);
        return GC_ERROR;
    }

    sn_initr(type, argv[0], strlen(argv[0]));
    sn_initz(response, "tunnel_response");
    sn_initz(request, "tunnel_request");

    if(sn_cmps(type, request)) {
        ret = gc_endpoint_request(gc, p, argv, argc);
        if(ret != GC_OK) {
            hm_log(LOG_ERR, &gc->log, "Tunnel request failed");
        }
    } else if(sn_cmps(type, response)) {
        ret = gc_tunnel_response(gc, p, argv, argc);
        if(ret != GC_OK) {
            hm_log(LOG_ERR, &gc->log, "Tunnel reponse failed");
        }
    }

    free(argv);

    return GC_OK;
}

static void gc_cloud_offline(struct gc_s *gc, struct proto_s *p)
{
    gc_endpoint_stop(&gc->log,
                     p->u.offline_set.address,
                     p->u.offline_set.cloud,
                     p->u.offline_set.device);

    gc_tunnel_stop(p->u.offline_set.address);
}

static void callback_error(struct client_ssl_s *c, enum gcerr_e error)
{
    struct gc_s *gc;

    gc = (struct gc_s *)c->base.gc;
    async_client_ssl_shutdown(c);
    ev_timer_again(gc->loop, &gc->connect_timer);
    (void)error;
}

static void callback_data(struct gc_s *gc, const void *buffer, const int nbuffer)
{
    struct proto_s p;
    sn src = { .s = (char *)buffer + 4, .n = nbuffer - 4 };
    if(deserialize(&p, &src) != 0) {
        hm_log(LOG_ERR, &gc->log, "Parsing failed");
        return;
    }

    hm_log(LOG_TRACE, &gc->log, "Received packet from upstream type: %d size: %d",
                                p.type, nbuffer);

    switch(p.type) {
        case ACCOUNT_LOGIN_REPLY:
            if(gc->callback_login)
                gc->callback_login(gc, p.u.account_login_reply.error);
        break;
        case DEVICE_PAIR_REPLY: {
            sn_initz(ok, "ok");
            if(sn_cmps(ok, p.u.device_pair_reply.error)) {
                sn list = p.u.device_pair_reply.list;

// WARNING: not alligned, endian ignorant
#define READ(m_var)\
            if(i < list.n) {\
                m_var.n = *(int *)(&((list.s)[i]));\
                /* Swap memory because of server high endian */\
                gc_swap_memory((void *)&m_var.n, sizeof(m_var.n));\
                m_var.s = &((list.s)[i + 4]);\
                i += sizeof(m_var.n) + m_var.n;\
            }

                int i;
                for(i = 0; i < list.n; i++) {
                    struct gc_device_pair_s pair;
                    sn_set(pair.cloud, p.u.device_pair_reply.cloud);
                    READ(pair.pid);
                    READ(pair.device);
                    READ(pair.port_local);
                    READ(pair.port_remote);
                    sn_set(pair.type, p.u.device_pair_reply.type);

                    if(gc->callback_device_pair)
                        gc->callback_device_pair(gc, &pair);
                }
            }
            }
        break;
        case MESSAGE_FROM: {
                gc_message_from(gc, &p);
            }
        break;
        case OFFLINE_SET: {
                gc_cloud_offline(gc, &p);
            }
        break;
        default:
            hm_log(LOG_TRACE, &gc->log, "Not handling packet type: %d size: %d", p.type, nbuffer);
        break;
    }
}

static void upstream_connect(struct ev_loop *loop, struct ev_timer *timer, int revents)
{
    struct gc_s *gc;
    gc = (struct gc_s *)timer->data;

    ev_timer_stop(loop, &gc->connect_timer);

    memset(&gc->client, 0, sizeof(gc->client));

    gc->client.base.loop = loop;
    gc->client.base.log  = &gc->log;

    gc->client.base.net.port = gc->port;
    snb_cpy_ds(gc->client.base.net.ip, gc->hostname);

    gc->client.callback_data  = callback_data;
    gc->client.callback_error = callback_error;

    async_client_ssl(gc);
    (void )revents;
}

int gc_deinit(struct gc_s *gc)
{
    if(gc->net.buf.s) free(gc->net.buf.s);

    FIPS_mode_set(0);
    ENGINE_cleanup();
    CONF_modules_unload(1);
    EVP_cleanup();
    CRYPTO_cleanup_all_ex_data();
    ERR_remove_state(0);
    ERR_free_strings();

    free(gc);

    ev_default_destroy();

    return 0;
}

static void sigh_terminate(int __attribute__ ((unused)) signo)
{
    if(gc_sigterm == 1) return;

    gc_sigterm = 1;
    //hm_log(LOG_TRACE, &gc->log, "Received SIGTERM");
    gc_force_stop();
}

static void gc_signals(struct gc_s *gc)
{
    struct sigaction act;

    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = SIG_IGN;

    if(sigaction(SIGPIPE, &act, NULL) < 0) {
        hm_log(LOG_CRIT, &gc->log, "Sigaction cannot be examined");
    }

    act.sa_flags = SA_NOCLDSTOP;

    act.sa_flags = 0;
    act.sa_handler = sigh_terminate;
    if(sigaction(SIGINT, &act, NULL) < 0) {
        hm_log(LOG_CRIT, &gc->log, "Unable to register SIGINT signal handler: %s", strerror(errno));
        exit(1);
    }

    if(sigaction(SIGTERM, &act, NULL) < 0) {
        hm_log(LOG_CRIT, &gc->log, "Unable to register SIGTERM signal handler: %s", strerror(errno));
        exit(1);
    }
}

static int config_init(struct gc_config_s *cfg, const char *filename)
{
    int ret;

    ret = gc_config_parse(cfg, filename);
    if(ret != GC_OK) {
        hm_log(LOG_CRIT, cfg->log, "Parsing config file [%s] failed", filename);
        return GC_ERROR;
    }

    assert(cfg->log);
    gc_config_dump(cfg);

    return GC_OK;
}

static int config_required(struct gc_config_s *cfg)
{
    assert(cfg);

    if(cfg->username.n == 0 || cfg->password.n == 0 || cfg->device.n == 0) {
        hm_log(LOG_CRIT, cfg->log, "Username, password nad device must be set");
        return GC_ERROR;
    }

    if(cfg->ntunnels == 0 && cfg->nallowed == 0) {
        hm_log(LOG_CRIT, cfg->log, "Neither tunnels nor allowed ports specified");
        return GC_ERROR;
    }

    if(cfg->ntunnels > 0 && cfg->nallowed > 0) cfg->type = GC_TYPE_HYBRID;
    else if(cfg->ntunnels > 0)                 cfg->type = GC_TYPE_CLIENT;
    else if(cfg->nallowed > 0)                 cfg->type = GC_TYPE_SERVER;
    else                                       return GC_ERROR;

    return GC_OK;
}

struct gc_s *gc_init(struct gc_init_s *init)
{
    struct gc_s *gc = NULL;

    assert(init);

    gc = gclocal = malloc(sizeof(*gc));
    memset(gc, 0, sizeof(*gc));

    if(hm_log_open(&gc->log, NULL, LOG_TRACE) != GC_OK) {
        return NULL;
    }

    gc->config.log = &gc->log;
    if(config_init(&gc->config, init->cfgfile) != GC_OK) {
        hm_log(LOG_CRIT, &gc->log, "Could not initialize config file");
        return NULL;
    }

    if(config_required(&gc->config) != GC_OK) {
        hm_log(LOG_CRIT, &gc->log, "Mandatory configuration parameters are missing");
        return NULL;
    }

    // Copy over initialization settings
    gc->loop            = init->loop;
    gc->state_changed   = init->state_changed;

    gc->callback_login       = init->callback_login;
    gc->callback_device_pair = init->callback_device_pair;

    gc->hostname = init->hostname;
    gc->port     = init->port;

    // Initialize signals
    gc_signals(gc);

    if(SSL_library_init() < 0) {
        hm_log(LOG_CRIT, &gc->log, "Could not initialize OpenSSL library");
        return NULL;
    }

    SSL_load_error_strings();

    // Try to connect to upstream every .repeat seconds
    ev_init(&gc->connect_timer, upstream_connect);
    gc->connect_timer.repeat = 2.0;
    gc->connect_timer.data = gc;
    ev_timer_again(gc->loop, &gc->connect_timer);

    return gc;
}

static void gc_upstream_force_stop(struct ev_loop *loop)
{
    ev_timer_stop(loop, &gclocal->connect_timer);

    async_client_ssl_shutdown(&gclocal->client);
}

void gc_force_stop()
{
    gc_config_free(&gclocal->config);
    gc_upstream_force_stop(gclocal->loop);
    gc_tunnel_stop_all();
    gc_endpoints_stop_all();
}

void gc_config_free(struct gc_config_s *cfg)
{
    json_object_put(cfg->jobj);
    free(cfg->content);
}
