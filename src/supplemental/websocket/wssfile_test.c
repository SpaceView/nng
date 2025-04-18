//
// Copyright 2024 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"
#include "nng/nng.h"

#include <nuts.h>

#ifdef NNG_SUPP_TLS

#define CACERT "wss_test_ca_cert.pem"
#define CERT_KEY "wss_test_cert_key.pem"

static void
init_dialer_wss_file(nng_dialer d)
{
	char           *tmpdir;
	char           *pth;
	nng_tls_config *c;

	NUTS_ASSERT((tmpdir = nni_plat_temp_dir()) != NULL);
	NUTS_ASSERT((pth = nni_file_join(tmpdir, CACERT)) != NULL);
	nng_strfree(tmpdir);
	NUTS_PASS(nni_file_put(pth, nuts_server_crt, strlen(nuts_server_crt)));
	NUTS_PASS(nng_tls_config_alloc(&c, NNG_TLS_MODE_CLIENT));
	NUTS_PASS(nng_tls_config_ca_file(c, pth));
	NUTS_PASS(nng_tls_config_server_name(c, "localhost"));
	NUTS_PASS(nng_dialer_set_tls(d, c));
	nni_file_delete(pth);
	nng_strfree(pth);
	nng_tls_config_free(c);
}

static void
init_listener_wss_file(nng_listener l)
{
	char           *tmpdir;
	char           *pth;
	char           *cert_key;
	nng_tls_config *c;

	NUTS_ASSERT((tmpdir = nni_plat_temp_dir()) != NULL);
	NUTS_ASSERT((pth = nni_file_join(tmpdir, CERT_KEY)) != NULL);
	nng_strfree(tmpdir);

	NUTS_PASS(nni_asprintf(
	    &cert_key, "%s\r\n%s\r\n", nuts_server_key, nuts_server_crt));

	NUTS_PASS(nni_file_put(pth, cert_key, strlen(cert_key)));
	nng_strfree(cert_key);
	NUTS_PASS(nng_tls_config_alloc(&c, NNG_TLS_MODE_SERVER));
	NUTS_PASS(nng_tls_config_cert_key_file(c, pth, pth));
	NUTS_PASS(nng_listener_set_tls(l, c));

	nni_file_delete(pth);
	nng_strfree(pth);
	nng_tls_config_free(c);
}

static void
test_invalid_verify(void)
{
	nng_socket   s1;
	nng_socket   s2;
	nng_listener l;
	nng_dialer   d;
	char         addr[40];
	int          port;

	(void) snprintf(addr, sizeof(addr), "wss4://:0/test");

	NUTS_PASS(nng_pair1_open(&s1));
	NUTS_PASS(nng_pair1_open(&s2));
	NUTS_PASS(nng_listener_create(&l, s1, addr));
	init_listener_wss_file(l);
	NUTS_PASS(nng_listener_start(l, 0));
	NUTS_PASS(nng_listener_get_int(l, NNG_OPT_TCP_BOUND_PORT, &port));

	nng_msleep(100);

	snprintf(addr, sizeof(addr), "wss://127.0.0.1:%u/test", port);

	// We find that sometimes this fails due to NNG_EPEERAUTH, but it
	// can also fail due to NNG_ECLOSED.  This seems to be timing
	// dependent, based on receive vs. send timing most likely.
	// Applications shouldn't really depend that much on this.
	int rv;

	NUTS_PASS(nng_dialer_create(&d, s2, addr));
	rv = nng_dialer_start(d, 0);

	NUTS_TRUE(rv != 0);
	NUTS_TRUE((rv == NNG_EPEERAUTH) || (rv == NNG_ECLOSED) ||
	    (rv == NNG_ECRYPTO));

	NUTS_CLOSE(s1);
	NUTS_CLOSE(s2);
}

static void
test_no_verify(void)
{
	nng_socket   s1;
	nng_socket   s2;
	nng_listener l;
	nng_dialer   d;
	char         addr[64];
	nng_msg     *msg;
	nng_pipe     p;
	bool         b;
	int          port;

	NUTS_ENABLE_LOG(NNG_LOG_DEBUG);
	NUTS_PASS(nng_pair1_open(&s1));
	NUTS_PASS(nng_pair1_open(&s2));
	NUTS_PASS(nng_socket_set_ms(s1, NNG_OPT_SENDTIMEO, 5000));
	NUTS_PASS(nng_socket_set_ms(s2, NNG_OPT_RECVTIMEO, 5000));

	port = nuts_next_port();
	(void) snprintf(addr, sizeof(addr), "wss4://:0/test");
	NUTS_PASS(nng_listener_create(&l, s1, addr));
	init_listener_wss_file(l);
	NUTS_PASS(nng_listener_start(l, 0));
	NUTS_PASS(nng_listener_get_int(l, NNG_OPT_TCP_BOUND_PORT, &port));

	nng_msleep(100);
	snprintf(addr, sizeof(addr), "wss://127.0.0.1:%u/test", port);
	NUTS_PASS(nng_dialer_create(&d, s2, addr));
	init_dialer_wss_file(d);

	NUTS_PASS(nng_dialer_start(d, 0));
	nng_msleep(100);

	NUTS_PASS(nng_send(s1, "hello", 6, 0));
	NUTS_PASS(nng_recvmsg(s2, &msg, 0));
	NUTS_ASSERT(msg != NULL);
	NUTS_TRUE(nng_msg_len(msg) == 6);
	NUTS_MATCH(nng_msg_body(msg), "hello");

	p = nng_msg_get_pipe(msg);
	NUTS_TRUE(nng_pipe_id(p) > 0);
	NUTS_PASS(nng_pipe_get_bool(p, NNG_OPT_TLS_VERIFIED, &b));
	// Server may have verified, us, or might not have.
	// This is dependent
	// NUTS_TRUE(b == false);

	nng_msg_free(msg);
	NUTS_CLOSE(s1);
	NUTS_CLOSE(s2);
}

static void
test_verify_works(void)
{
	nng_socket   s1;
	nng_socket   s2;
	nng_listener l;
	nng_dialer   d;
	char         addr[NNG_MAXADDRLEN];
	nng_msg     *msg;
	nng_pipe     p;
	bool         b;
	int          port;

	NUTS_PASS(nng_pair1_open(&s1));
	NUTS_PASS(nng_pair1_open(&s2));
	NUTS_PASS(nng_socket_set_ms(s1, NNG_OPT_SENDTIMEO, 5000));
	NUTS_PASS(nng_socket_set_ms(s2, NNG_OPT_RECVTIMEO, 5000));
	port = nuts_next_port();
	(void) snprintf(addr, sizeof(addr), "wss4://:0/test");
	NUTS_PASS(nng_listener_create(&l, s1, addr));
	init_listener_wss_file(l);
	NUTS_PASS(nng_listener_start(l, 0));
	NUTS_PASS(nng_listener_get_int(l, NNG_OPT_TCP_BOUND_PORT, &port));

	// It can take a bit for the listener to start up in clouds.
	nng_msleep(200);
	snprintf(addr, sizeof(addr), "wss4://localhost:%u/test", port);
	NUTS_PASS(nng_dialer_create(&d, s2, addr));
	init_dialer_wss_file(d);

	NUTS_PASS(nng_dialer_start(d, 0));
	nng_msleep(100);

	NUTS_SEND(s1, "hello");
	NUTS_PASS(nng_recvmsg(s2, &msg, 0));
	NUTS_ASSERT(msg != NULL);
	NUTS_TRUE(nng_msg_len(msg) == 6);
	NUTS_MATCH(nng_msg_body(msg), "hello");

	p = nng_msg_get_pipe(msg);
	NUTS_TRUE(nng_pipe_id(p) > 0);
	NUTS_PASS(nng_pipe_get_bool(p, NNG_OPT_TLS_VERIFIED, &b));
	NUTS_TRUE(b == true);

	nng_msg_free(msg);
	NUTS_CLOSE(s1);
	NUTS_CLOSE(s2);
}

static void
test_cert_file_not_present(void)
{
	nng_tls_config *c;

	NUTS_PASS(nng_tls_config_alloc(&c, NNG_TLS_MODE_SERVER));
	NUTS_FAIL(nng_tls_config_cert_key_file(
	              c, "no-such-file.pem", "no-such-file.pem"),
	    NNG_ENOENT);
	nng_tls_config_free(c);
}

static void
test_tls_config(void)
{
	nng_socket      s1;
	nng_socket      s2;
	nng_listener    l;
	nng_dialer      d;
	char            addr[40];
	nng_tls_config *cfg;
	int             port;

	(void) snprintf(addr, sizeof(addr), "wss4://:0/test");

	NUTS_PASS(nng_pair1_open(&s1));
	NUTS_PASS(nng_pair1_open(&s2));
	NUTS_PASS(nng_listener_create(&l, s1, addr));
	NUTS_PASS(nng_listener_get_tls(l, &cfg));
	nng_tls_config_hold(cfg);
	NUTS_TRUE(cfg != NULL);

	init_listener_wss_file(l);
	NUTS_PASS(nng_listener_start(l, 0));
	NUTS_PASS(nng_listener_get_int(l, NNG_OPT_TCP_BOUND_PORT, &port));

	// make sure we cannot change the auth mode while running

	NUTS_FAIL(nng_listener_set_tls(l, cfg), NNG_EBUSY);
	nng_tls_config_free(cfg);

	NUTS_PASS(nng_listener_get_tls(l, &cfg));

	NUTS_FAIL(
	    nng_tls_config_auth_mode(cfg, NNG_TLS_AUTH_MODE_NONE), NNG_EBUSY);

	nng_msleep(100);

	snprintf(addr, sizeof(addr), "wss://127.0.0.1:%u/test", port);

	// We find that sometimes this fails due to NNG_EPEERAUTH, but it
	// can also fail due to NNG_ECLOSED.  This seems to be timing
	// dependent, based on receive vs. send timing most likely.
	// Applications shouldn't really depend that much on this.
	int rv;

	NUTS_PASS(nng_dialer_create(&d, s2, addr));
	rv = nng_dialer_start(d, 0);
	NUTS_PASS(nng_dialer_get_tls(d, &cfg));
	NUTS_FAIL(nng_dialer_set_tls(d, cfg), NNG_EBUSY);

	NUTS_TRUE(rv != 0);
	NUTS_TRUE((rv == NNG_EPEERAUTH) || (rv == NNG_ECLOSED) ||
	    (rv == NNG_ECRYPTO));

	NUTS_CLOSE(s1);
	NUTS_CLOSE(s2);
}

#endif

NUTS_TESTS = {
#ifdef NNG_SUPP_TLS
	{ "wss file invalid verify", test_invalid_verify },
	{ "wss file no verify", test_no_verify },
	{ "wss file verify works", test_verify_works },
	{ "wss file ca cert missing", test_cert_file_not_present },
	{ "wss tls config", test_tls_config },
#endif
	{ NULL, NULL },
};
