/* $OpenBSD: tlsexttest.c,v 1.12 2017/08/12 23:39:24 beck Exp $ */
/*
 * Copyright (c) 2017 Joel Sing <jsing@openbsd.org>
 * Copyright (c) 2017 Doug Hogan <doug@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <err.h>

#include "ssl_locl.h"

#include "bytestring.h"
#include "ssl_tlsext.h"

static void
hexdump(const unsigned char *buf, size_t len)
{
	size_t i;

	for (i = 1; i <= len; i++)
		fprintf(stderr, " 0x%02hhx,%s", buf[i - 1], i % 8 ? "" : "\n");

	fprintf(stderr, "\n");
}

static void
hexdump2(const uint16_t *buf, size_t len)
{
	size_t i;

	for (i = 1; i <= len / 2; i++)
		fprintf(stderr, " 0x%04hx,%s", buf[i - 1], i % 8 ? "" : "\n");

	fprintf(stderr, "\n");
}

static void
compare_data(const uint8_t *recv, size_t recv_len, const uint8_t *expect,
    size_t expect_len)
{
	fprintf(stderr, "received:\n");
	hexdump(recv, recv_len);

	fprintf(stderr, "test data:\n");
	hexdump(expect, expect_len);
}

static void
compare_data2(const uint16_t *recv, size_t recv_len, const uint16_t *expect,
    size_t expect_len)
{
	fprintf(stderr, "received:\n");
	hexdump2(recv, recv_len);

	fprintf(stderr, "test data:\n");
	hexdump2(expect, expect_len);
}

#define FAIL(msg, ...)						\
do {								\
	fprintf(stderr, "[%s:%d] FAIL: ", __FILE__, __LINE__);	\
	fprintf(stderr, msg, ##__VA_ARGS__);			\
} while(0)

/*
 * Supported Elliptic Curves - RFC 4492 section 5.1.1.
 *
 * This extension is only used by the client.
 */

static uint8_t tlsext_ec_clienthello_default[] = {
	0x00, 0x06,
	0x00, 0x1d,  /* X25519 (29) */
	0x00, 0x17,  /* secp256r1 (23) */
	0x00, 0x18   /* secp384r1 (24) */
};

static uint16_t tlsext_ec_clienthello_secp384r1_val[] = {
	0x0018   /* tls1_ec_nid2curve_id(NID_secp384r1) */
};
static uint8_t tlsext_ec_clienthello_secp384r1[] = {
	0x00, 0x02,
	0x00, 0x18  /* secp384r1 (24) */
};

/* Example from RFC 4492 section 5.1.1 */
static uint16_t tlsext_ec_clienthello_nistp192and224_val[] = {
	0x0013,  /* tls1_ec_nid2curve_id(NID_X9_62_prime192v1) */
	0x0015   /* tls1_ec_nid2curve_id(NID_secp224r1) */
};
static uint8_t tlsext_ec_clienthello_nistp192and224[] = {
	0x00, 0x04,
	0x00, 0x13, /* secp192r1 aka NIST P-192 */
	0x00, 0x15  /* secp224r1 aka NIST P-224 */
};

static int
test_tlsext_ec_clienthello(void)
{
	unsigned char *data = NULL;
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	size_t dlen;
	int failure, alert;
	CBB cbb;
	CBS cbs;

	failure = 1;

	CBB_init(&cbb, 0);

	if ((ssl_ctx = SSL_CTX_new(TLS_client_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	/*
	 * Default ciphers include EC so we need it by default.
	 */
	if (!tlsext_ec_clienthello_needs(ssl)) {
		FAIL("clienthello should need Ellipticcurves for default "
		    "ciphers\n");
		goto err;
	}

	/*
	 * Exclude cipher suites so we can test not including it.
	 */
	if (!SSL_set_cipher_list(ssl, "TLSv1.2:!ECDHE:!ECDSA")) {
		FAIL("clienthello should be able to set cipher list\n");
		goto err;
	}
	if (tlsext_ec_clienthello_needs(ssl)) {
		FAIL("clienthello should not need Ellipticcurves\n");
		goto err;
	}

	/*
	 * Use libtls default for the rest of the testing
	 */
	if (!SSL_set_cipher_list(ssl, "TLSv1.2+AEAD+ECDHE")) {
		FAIL("clienthello should be able to set cipher list\n");
		goto err;
	}
	if (!tlsext_ec_clienthello_needs(ssl)) {
		FAIL("clienthello should need Ellipticcurves\n");
		goto err;
	}

	/*
	 * Test with a session secp384r1.  The default is used instead.
	 */
	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	if ((SSI(ssl)->tlsext_supportedgroups = malloc(sizeof(uint16_t)))
	    == NULL) {
		FAIL("client could not malloc\n");
		goto err;
	}
	SSI(ssl)->tlsext_supportedgroups[0] = tls1_ec_nid2curve_id(NID_secp384r1);
	SSI(ssl)->tlsext_supportedgroups_length = 1;

	if (!tlsext_ec_clienthello_needs(ssl)) {
		FAIL("clienthello should need Ellipticcurves\n");
		goto err;
	}

	if (!tlsext_ec_clienthello_build(ssl, &cbb)) {
		FAIL("clienthello failed to build Ellipticcurves\n");
		goto err;
	}

	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	if (dlen != sizeof(tlsext_ec_clienthello_default)) {
		FAIL("got clienthello Ellipticcurves with length %zu, "
		    "want length %zu\n", dlen,
		    sizeof(tlsext_ec_clienthello_default));
		compare_data(data, dlen, tlsext_ec_clienthello_default,
		    sizeof(tlsext_ec_clienthello_default));
		goto err;
	}

	if (memcmp(data, tlsext_ec_clienthello_default, dlen) != 0) {
		FAIL("clienthello Ellipticcurves differs:\n");
		compare_data(data, dlen, tlsext_ec_clienthello_default,
		    sizeof(tlsext_ec_clienthello_default));
		goto err;
	}

	/*
	 * Test parsing secp384r1
	 */
	CBB_cleanup(&cbb);
	CBB_init(&cbb, 0);
	free(data);
	data = NULL;

	SSL_SESSION_free(ssl->session);
	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	CBS_init(&cbs, tlsext_ec_clienthello_secp384r1,
	    sizeof(tlsext_ec_clienthello_secp384r1));
	if (!tlsext_ec_clienthello_parse(ssl, &cbs, &alert)) {
		FAIL("failed to parse clienthello Ellipticcurves\n");
		goto err;
	}

	if (SSI(ssl)->tlsext_supportedgroups_length !=
	    sizeof(tlsext_ec_clienthello_secp384r1_val) / sizeof(uint16_t)) {
		FAIL("no tlsext_ellipticcurves from clienthello "
		    "Ellipticcurves\n");
		goto err;
	}

	if (memcmp(SSI(ssl)->tlsext_supportedgroups,
	    tlsext_ec_clienthello_secp384r1_val,
	    sizeof(tlsext_ec_clienthello_secp384r1_val)) != 0) {
		FAIL("clienthello had an incorrect Ellipticcurves "
		    "entry\n");
		compare_data2(SSI(ssl)->tlsext_supportedgroups,
		    SSI(ssl)->tlsext_supportedgroups_length * 2,
		    tlsext_ec_clienthello_secp384r1_val,
		    sizeof(tlsext_ec_clienthello_secp384r1_val));
		goto err;
	}

	/*
	 * Use a custom order.
	 */
	CBB_cleanup(&cbb);
	CBB_init(&cbb, 0);

	SSL_SESSION_free(ssl->session);
	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	if ((ssl->internal->tlsext_supportedgroups = malloc(sizeof(uint16_t) * 2)) == NULL) {
		FAIL("client could not malloc\n");
		goto err;
	}
	ssl->internal->tlsext_supportedgroups[0] = tls1_ec_nid2curve_id(NID_X9_62_prime192v1);
	ssl->internal->tlsext_supportedgroups[1] = tls1_ec_nid2curve_id(NID_secp224r1);
	ssl->internal->tlsext_supportedgroups_length = 2;

	if (!tlsext_ec_clienthello_needs(ssl)) {
		FAIL("clienthello should need Ellipticcurves\n");
		goto err;
	}

	if (!tlsext_ec_clienthello_build(ssl, &cbb)) {
		FAIL("clienthello failed to build Ellipticcurves\n");
		goto err;
	}

	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	if (dlen != sizeof(tlsext_ec_clienthello_nistp192and224)) {
		FAIL("got clienthello Ellipticcurves with length %zu, "
		    "want length %zu\n", dlen,
		    sizeof(tlsext_ec_clienthello_nistp192and224));
		fprintf(stderr, "received:\n");
		hexdump(data, dlen);
		fprintf(stderr, "test data:\n");
		hexdump(tlsext_ec_clienthello_nistp192and224,
		    sizeof(tlsext_ec_clienthello_nistp192and224));
		goto err;
	}

	if (memcmp(data, tlsext_ec_clienthello_nistp192and224, dlen) != 0) {
		FAIL("clienthello Ellipticcurves differs:\n");
		fprintf(stderr, "received:\n");
		hexdump(data, dlen);
		fprintf(stderr, "test data:\n");
		hexdump(tlsext_ec_clienthello_nistp192and224,
		    sizeof(tlsext_ec_clienthello_nistp192and224));
		goto err;
	}

	/*
	 * Parse non-default curves to session.
	 */
	CBB_cleanup(&cbb);
	CBB_init(&cbb, 0);
	free(data);
	data = NULL;

	SSL_SESSION_free(ssl->session);
	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	/* Reset back to the default list. */
	free(ssl->internal->tlsext_supportedgroups);
	ssl->internal->tlsext_supportedgroups = NULL;
	ssl->internal->tlsext_supportedgroups_length = 0;

	CBS_init(&cbs, tlsext_ec_clienthello_nistp192and224,
	    sizeof(tlsext_ec_clienthello_nistp192and224));
	if (!tlsext_ec_clienthello_parse(ssl, &cbs, &alert)) {
		FAIL("failed to parse clienthello Ellipticcurves\n");
		goto err;
	}

	if (SSI(ssl)->tlsext_supportedgroups_length !=
	    sizeof(tlsext_ec_clienthello_nistp192and224_val) / sizeof(uint16_t)) {
		FAIL("no tlsext_ellipticcurves from clienthello "
		    "Ellipticcurves\n");
		goto err;
	}

	if (memcmp(SSI(ssl)->tlsext_supportedgroups,
	    tlsext_ec_clienthello_nistp192and224_val,
	    sizeof(tlsext_ec_clienthello_nistp192and224_val)) != 0) {
		FAIL("clienthello had an incorrect Ellipticcurves entry\n");
		compare_data2(SSI(ssl)->tlsext_supportedgroups,
		    SSI(ssl)->tlsext_supportedgroups_length * 2,
		    tlsext_ec_clienthello_nistp192and224_val,
		    sizeof(tlsext_ec_clienthello_nistp192and224_val));
		goto err;
	}

	failure = 0;

 err:
	CBB_cleanup(&cbb);
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);
	free(data);

	return (failure);
}


/* elliptic_curves is only used by the client so this doesn't test much. */
static int
test_tlsext_ec_serverhello(void)
{
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	int failure;

	failure = 1;

	if ((ssl_ctx = SSL_CTX_new(TLS_server_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	if (tlsext_ec_serverhello_needs(ssl)) {
		FAIL("serverhello should not need elliptic_curves\n");
		goto err;
	}

	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	if (tlsext_ec_serverhello_needs(ssl)) {
		FAIL("serverhello should not need elliptic_curves\n");
		goto err;
	}

	failure = 0;

 err:
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);

	return (failure);

}

/*
 * Supported Point Formats - RFC 4492 section 5.1.2.
 *
 * Examples are from the RFC.  Both client and server have the same build and
 * parse but the needs differ.
 */

static uint8_t tlsext_ecpf_hello_uncompressed_val[] = {
	TLSEXT_ECPOINTFORMAT_uncompressed
};
static uint8_t tlsext_ecpf_hello_uncompressed[] = {
	0x01,
	0x00 /* TLSEXT_ECPOINTFORMAT_uncompressed */
};

static uint8_t tlsext_ecpf_hello_prime[] = {
	0x01,
	0x01 /* TLSEXT_ECPOINTFORMAT_ansiX962_compressed_prime */
};

static uint8_t tlsext_ecpf_hello_prefer_order_val[] = {
	TLSEXT_ECPOINTFORMAT_ansiX962_compressed_prime,
	TLSEXT_ECPOINTFORMAT_uncompressed,
	TLSEXT_ECPOINTFORMAT_ansiX962_compressed_char2
};
static uint8_t tlsext_ecpf_hello_prefer_order[] = {
	0x03,
	0x01, /* TLSEXT_ECPOINTFORMAT_ansiX962_compressed_prime */
	0x00, /* TLSEXT_ECPOINTFORMAT_uncompressed */
	0x02  /* TLSEXT_ECPOINTFORMAT_ansiX962_compressed_char2 */
};

static int
test_tlsext_ecpf_clienthello(void)
{
	uint8_t *data = NULL;
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	size_t dlen;
	int failure, alert;
	CBB cbb;
	CBS cbs;

	failure = 1;

	CBB_init(&cbb, 0);

	if ((ssl_ctx = SSL_CTX_new(TLS_client_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	/*
	 * Default ciphers include EC so we need it by default.
	 */
	if (!tlsext_ecpf_clienthello_needs(ssl)) {
		FAIL("clienthello should need ECPointFormats for default "
		    "ciphers\n");
		goto err;
	}

	/*
	 * Exclude EC cipher suites so we can test not including it.
	 */
	if (!SSL_set_cipher_list(ssl, "ALL:!ECDHE:!ECDH")) {
		FAIL("clienthello should be able to set cipher list\n");
		goto err;
	}
	if (tlsext_ecpf_clienthello_needs(ssl)) {
		FAIL("clienthello should not need ECPointFormats\n");
		goto err;
	}

	/*
	 * Use libtls default for the rest of the testing
	 */
	if (!SSL_set_cipher_list(ssl, "TLSv1.2+AEAD+ECDHE")) {
		FAIL("clienthello should be able to set cipher list\n");
		goto err;
	}
	if (!tlsext_ecpf_clienthello_needs(ssl)) {
		FAIL("clienthello should need ECPointFormats\n");
		goto err;
	}

	/*
	 * The default ECPointFormats should only have uncompressed
	 */
	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	if (!tlsext_ecpf_clienthello_build(ssl, &cbb)) {
		FAIL("clienthello failed to build ECPointFormats\n");
		goto err;
	}

	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	if (dlen != sizeof(tlsext_ecpf_hello_uncompressed)) {
		FAIL("got clienthello ECPointFormats with length %zu, "
		    "want length %zu\n", dlen,
		    sizeof(tlsext_ecpf_hello_uncompressed));
		compare_data(data, dlen, tlsext_ecpf_hello_uncompressed,
		    sizeof(tlsext_ecpf_hello_uncompressed));
		goto err;
	}

	if (memcmp(data, tlsext_ecpf_hello_uncompressed, dlen) != 0) {
		FAIL("clienthello ECPointFormats differs:\n");
		compare_data(data, dlen, tlsext_ecpf_hello_uncompressed,
		    sizeof(tlsext_ecpf_hello_uncompressed));
		goto err;
	}

	/*
	 * Make sure we can parse the default.
	 */
	CBB_cleanup(&cbb);
	CBB_init(&cbb, 0);
	free(data);
	data = NULL;

	SSL_SESSION_free(ssl->session);
	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	CBS_init(&cbs, tlsext_ecpf_hello_uncompressed,
	    sizeof(tlsext_ecpf_hello_uncompressed));
	if (!tlsext_ecpf_clienthello_parse(ssl, &cbs, &alert)) {
		FAIL("failed to parse clienthello ECPointFormats\n");
		goto err;
	}

	if (SSI(ssl)->tlsext_ecpointformatlist_length !=
	    sizeof(tlsext_ecpf_hello_uncompressed_val)) {
		FAIL("no tlsext_ecpointformats from clienthello "
		    "ECPointFormats\n");
		goto err;
	}

	if (memcmp(SSI(ssl)->tlsext_ecpointformatlist,
	    tlsext_ecpf_hello_uncompressed_val,
	    sizeof(tlsext_ecpf_hello_uncompressed_val)) != 0) {
		FAIL("clienthello had an incorrect ECPointFormats entry\n");
		goto err;
	}

	/*
	 * Test with a custom order.
	 */
	CBB_cleanup(&cbb);
	CBB_init(&cbb, 0);
	free(data);
	data = NULL;

	SSL_SESSION_free(ssl->session);
	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	if ((ssl->internal->tlsext_ecpointformatlist = malloc(sizeof(uint8_t) * 3)) == NULL) {
		FAIL("client could not malloc\n");
		goto err;
	}
	ssl->internal->tlsext_ecpointformatlist[0] = TLSEXT_ECPOINTFORMAT_ansiX962_compressed_prime;
	ssl->internal->tlsext_ecpointformatlist[1] = TLSEXT_ECPOINTFORMAT_uncompressed;
	ssl->internal->tlsext_ecpointformatlist[2] = TLSEXT_ECPOINTFORMAT_ansiX962_compressed_char2;
	ssl->internal->tlsext_ecpointformatlist_length = 3;

	if (!tlsext_ecpf_clienthello_needs(ssl)) {
		FAIL("clienthello should need ECPointFormats with a custom "
		    "format\n");
		goto err;
	}

	if (!tlsext_ecpf_clienthello_build(ssl, &cbb)) {
		FAIL("clienthello failed to build ECPointFormats\n");
		goto err;
	}

	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	if (dlen != sizeof(tlsext_ecpf_hello_prefer_order)) {
		FAIL("got clienthello ECPointFormats with length %zu, "
		    "want length %zu\n", dlen,
		    sizeof(tlsext_ecpf_hello_prefer_order));
		compare_data(data, dlen, tlsext_ecpf_hello_prefer_order,
		    sizeof(tlsext_ecpf_hello_prefer_order));
		goto err;
	}

	if (memcmp(data, tlsext_ecpf_hello_prefer_order, dlen) != 0) {
		FAIL("clienthello ECPointFormats differs:\n");
		compare_data(data, dlen, tlsext_ecpf_hello_prefer_order,
		    sizeof(tlsext_ecpf_hello_prefer_order));
		goto err;
	}

	/*
	 * Make sure that we can parse this custom order.
	 */
	CBB_cleanup(&cbb);
	CBB_init(&cbb, 0);
	free(data);
	data = NULL;

	SSL_SESSION_free(ssl->session);
	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	/* Reset the custom list so we go back to the default uncompressed. */
	free(ssl->internal->tlsext_ecpointformatlist);
	ssl->internal->tlsext_ecpointformatlist = NULL;
	ssl->internal->tlsext_ecpointformatlist_length = 0;

	CBS_init(&cbs, tlsext_ecpf_hello_prefer_order,
	    sizeof(tlsext_ecpf_hello_prefer_order));
	if (!tlsext_ecpf_clienthello_parse(ssl, &cbs, &alert)) {
		FAIL("failed to parse clienthello ECPointFormats\n");
		goto err;
	}

	if (SSI(ssl)->tlsext_ecpointformatlist_length !=
	    sizeof(tlsext_ecpf_hello_prefer_order_val)) {
		FAIL("no tlsext_ecpointformats from clienthello "
		    "ECPointFormats\n");
		goto err;
	}

	if (memcmp(SSI(ssl)->tlsext_ecpointformatlist,
	    tlsext_ecpf_hello_prefer_order_val,
	    sizeof(tlsext_ecpf_hello_prefer_order_val)) != 0) {
		FAIL("clienthello had an incorrect ECPointFormats entry\n");
		goto err;
	}


	failure = 0;

 err:
	CBB_cleanup(&cbb);
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);
	free(data);

	return (failure);
}

static int
test_tlsext_ecpf_serverhello(void)
{
	uint8_t *data = NULL;
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	size_t dlen;
	int failure, alert;
	CBB cbb;
	CBS cbs;

	failure = 1;

	CBB_init(&cbb, 0);

	if ((ssl_ctx = SSL_CTX_new(TLS_server_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	/* Setup the state so we can call needs. */
	if ((S3I(ssl)->hs.new_cipher =
	    ssl3_get_cipher_by_id(TLS1_CK_ECDHE_ECDSA_CHACHA20_POLY1305))
	    == NULL) {
		FAIL("serverhello cannot find cipher\n");
		goto err;
	}
	if ((SSI(ssl)->tlsext_ecpointformatlist = malloc(sizeof(uint8_t)))
	    == NULL) {
		FAIL("server could not malloc\n");
		goto err;
	}
	SSI(ssl)->tlsext_ecpointformatlist[0] = TLSEXT_ECPOINTFORMAT_ansiX962_compressed_prime;
	SSI(ssl)->tlsext_ecpointformatlist_length = 1;

	if (!tlsext_ecpf_serverhello_needs(ssl)) {
		FAIL("serverhello should need ECPointFormats now\n");
		goto err;
	}

	/*
	 * The server will ignore the session list and use either a custom
	 * list or the default (uncompressed).
	 */
	if (!tlsext_ecpf_serverhello_build(ssl, &cbb)) {
		FAIL("serverhello failed to build ECPointFormats\n");
		goto err;
	}

	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	if (dlen != sizeof(tlsext_ecpf_hello_uncompressed)) {
		FAIL("got serverhello ECPointFormats with length %zu, "
		    "want length %zu\n", dlen,
		    sizeof(tlsext_ecpf_hello_uncompressed));
		compare_data(data, dlen, tlsext_ecpf_hello_uncompressed,
		    sizeof(tlsext_ecpf_hello_uncompressed));
		goto err;
	}

	if (memcmp(data, tlsext_ecpf_hello_uncompressed, dlen) != 0) {
		FAIL("serverhello ECPointFormats differs:\n");
		compare_data(data, dlen, tlsext_ecpf_hello_uncompressed,
		    sizeof(tlsext_ecpf_hello_uncompressed));
		goto err;
	}

	/*
	 * Cannot parse a non-default list without at least uncompressed.
	 */
	CBB_cleanup(&cbb);
	CBB_init(&cbb, 0);
	free(data);
	data = NULL;

	SSL_SESSION_free(ssl->session);
	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	CBS_init(&cbs, tlsext_ecpf_hello_prime,
	    sizeof(tlsext_ecpf_hello_prime));
	if (tlsext_ecpf_serverhello_parse(ssl, &cbs, &alert)) {
		FAIL("must include uncompressed in serverhello ECPointFormats\n");
		goto err;
	}

	/*
	 * Test with a custom order that replaces the default uncompressed.
	 */
	CBB_cleanup(&cbb);
	CBB_init(&cbb, 0);
	free(data);
	data = NULL;

	SSL_SESSION_free(ssl->session);
	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	/* Add a session list even though it will be ignored. */
	if ((SSI(ssl)->tlsext_ecpointformatlist = malloc(sizeof(uint8_t)))
	    == NULL) {
		FAIL("server could not malloc\n");
		goto err;
	}
	SSI(ssl)->tlsext_ecpointformatlist[0] = TLSEXT_ECPOINTFORMAT_ansiX962_compressed_char2;
	SSI(ssl)->tlsext_ecpointformatlist_length = 1;

	/* Replace the default list with a custom one. */
	if ((ssl->internal->tlsext_ecpointformatlist = malloc(sizeof(uint8_t) * 3)) == NULL) {
		FAIL("server could not malloc\n");
		goto err;
	}
	ssl->internal->tlsext_ecpointformatlist[0] = TLSEXT_ECPOINTFORMAT_ansiX962_compressed_prime;
	ssl->internal->tlsext_ecpointformatlist[1] = TLSEXT_ECPOINTFORMAT_uncompressed;
	ssl->internal->tlsext_ecpointformatlist[2] = TLSEXT_ECPOINTFORMAT_ansiX962_compressed_char2;
	ssl->internal->tlsext_ecpointformatlist_length = 3;

	if (!tlsext_ecpf_serverhello_needs(ssl)) {
		FAIL("serverhello should need ECPointFormats\n");
		goto err;
	}

	if (!tlsext_ecpf_serverhello_build(ssl, &cbb)) {
		FAIL("serverhello failed to build ECPointFormats\n");
		goto err;
	}

	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	if (dlen != sizeof(tlsext_ecpf_hello_prefer_order)) {
		FAIL("got serverhello ECPointFormats with length %zu, "
		    "want length %zu\n", dlen,
		    sizeof(tlsext_ecpf_hello_prefer_order));
		compare_data(data, dlen, tlsext_ecpf_hello_prefer_order,
		    sizeof(tlsext_ecpf_hello_prefer_order));
		goto err;
	}

	if (memcmp(data, tlsext_ecpf_hello_prefer_order, dlen) != 0) {
		FAIL("serverhello ECPointFormats differs:\n");
		compare_data(data, dlen, tlsext_ecpf_hello_prefer_order,
		    sizeof(tlsext_ecpf_hello_prefer_order));
		goto err;
	}

	/*
	 * Should be able to parse the custom list into a session list.
	 */
	CBB_cleanup(&cbb);
	CBB_init(&cbb, 0);
	free(data);
	data = NULL;

	SSL_SESSION_free(ssl->session);
	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	/* Reset back to the default (uncompressed) */
	free(ssl->internal->tlsext_ecpointformatlist);
	ssl->internal->tlsext_ecpointformatlist = NULL;
	ssl->internal->tlsext_ecpointformatlist_length = 0;

	CBS_init(&cbs, tlsext_ecpf_hello_prefer_order,
	    sizeof(tlsext_ecpf_hello_prefer_order));
	if (!tlsext_ecpf_serverhello_parse(ssl, &cbs, &alert)) {
		FAIL("failed to parse serverhello ECPointFormats\n");
		goto err;
	}

	if (SSI(ssl)->tlsext_ecpointformatlist_length !=
	    sizeof(tlsext_ecpf_hello_prefer_order_val)) {
		FAIL("no tlsext_ecpointformats from serverhello "
		    "ECPointFormats\n");
		goto err;
	}

	if (memcmp(SSI(ssl)->tlsext_ecpointformatlist,
	    tlsext_ecpf_hello_prefer_order_val,
	    sizeof(tlsext_ecpf_hello_prefer_order_val)) != 0) {
		FAIL("serverhello had an incorrect ECPointFormats entry\n");
		goto err;
	}

	failure = 0;

 err:
	CBB_cleanup(&cbb);
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);
	free(data);

	return (failure);
}

/*
 * Renegotiation Indication - RFC 5746.
 */

static unsigned char tlsext_ri_prev_client[] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

static unsigned char tlsext_ri_prev_server[] = {
	0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
	0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
};

static unsigned char tlsext_ri_clienthello[] = {
	0x10,
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

static unsigned char tlsext_ri_serverhello[] = {
	0x20,
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
	0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
	0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
};

static int
test_tlsext_ri_clienthello(void)
{
	unsigned char *data = NULL;
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	int failure;
	size_t dlen;
	int alert;
	CBB cbb;
	CBS cbs;

	failure = 1;

	CBB_init(&cbb, 0);

	if ((ssl_ctx = SSL_CTX_new(TLSv1_2_client_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	if (tlsext_ri_clienthello_needs(ssl)) {
		FAIL("clienthello should not need RI\n");
		goto err;
	}

	if (!SSL_renegotiate(ssl)) {
		FAIL("client failed to set renegotiate\n");
		goto err;
	}

	if (!tlsext_ri_clienthello_needs(ssl)) {
		FAIL("clienthello should need RI\n");
		goto err;
	}

	memcpy(S3I(ssl)->previous_client_finished, tlsext_ri_prev_client,
	    sizeof(tlsext_ri_prev_client));
	S3I(ssl)->previous_client_finished_len = sizeof(tlsext_ri_prev_client);

	S3I(ssl)->renegotiate_seen = 0;

	if (!tlsext_ri_clienthello_build(ssl, &cbb)) {
		FAIL("clienthello failed to build RI\n");
		goto err;
	}

	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	if (dlen != sizeof(tlsext_ri_clienthello)) {
		FAIL("got clienthello RI with length %zu, "
		    "want length %zu\n", dlen, sizeof(tlsext_ri_clienthello));
		goto err;
	}

	if (memcmp(data, tlsext_ri_clienthello, dlen) != 0) {
		FAIL("clienthello RI differs:\n");
		fprintf(stderr, "received:\n");
		hexdump(data, dlen);
		fprintf(stderr, "test data:\n");
		hexdump(tlsext_ri_clienthello, sizeof(tlsext_ri_clienthello));
		goto err;
	}

	CBS_init(&cbs, tlsext_ri_clienthello, sizeof(tlsext_ri_clienthello));
	if (!tlsext_ri_clienthello_parse(ssl, &cbs, &alert)) {
		FAIL("failed to parse clienthello RI\n");
		goto err;
	}

	if (S3I(ssl)->renegotiate_seen != 1) {
		FAIL("renegotiate seen not set\n");
		goto err;
	}
        if (S3I(ssl)->send_connection_binding != 1) {
		FAIL("send connection binding not set\n");
		goto err;
	}

	memset(S3I(ssl)->previous_client_finished, 0,
	    sizeof(S3I(ssl)->previous_client_finished));

	S3I(ssl)->renegotiate_seen = 0;

	CBS_init(&cbs, tlsext_ri_clienthello, sizeof(tlsext_ri_clienthello));
	if (tlsext_ri_clienthello_parse(ssl, &cbs, &alert)) {
		FAIL("parsed invalid clienthello RI\n");
		failure = 1;
		goto err;
	}

	if (S3I(ssl)->renegotiate_seen == 1) {
		FAIL("renegotiate seen set\n");
		goto err;
	}

	failure = 0;

 err:
	CBB_cleanup(&cbb);
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);
	free(data);

	return (failure);
}

static int
test_tlsext_ri_serverhello(void)
{
	unsigned char *data = NULL;
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	int failure;
	size_t dlen;
	int alert;
	CBB cbb;
	CBS cbs;

	failure = 1;

	CBB_init(&cbb, 0);

	if ((ssl_ctx = SSL_CTX_new(TLS_server_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	if (tlsext_ri_serverhello_needs(ssl)) {
		FAIL("serverhello should not need RI\n");
		goto err;
	}

        S3I(ssl)->send_connection_binding = 1;

	if (!tlsext_ri_serverhello_needs(ssl)) {
		FAIL("serverhello should need RI\n");
		goto err;
	}

	memcpy(S3I(ssl)->previous_client_finished, tlsext_ri_prev_client,
	    sizeof(tlsext_ri_prev_client));
	S3I(ssl)->previous_client_finished_len = sizeof(tlsext_ri_prev_client);

	memcpy(S3I(ssl)->previous_server_finished, tlsext_ri_prev_server,
	    sizeof(tlsext_ri_prev_server));
	S3I(ssl)->previous_server_finished_len = sizeof(tlsext_ri_prev_server);

	S3I(ssl)->renegotiate_seen = 0;

	if (!tlsext_ri_serverhello_build(ssl, &cbb)) {
		FAIL("serverhello failed to build RI\n");
		goto err;
	}

	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	if (dlen != sizeof(tlsext_ri_serverhello)) {
		FAIL("got serverhello RI with length %zu, "
		    "want length %zu\n", dlen, sizeof(tlsext_ri_serverhello));
		goto err;
	}

	if (memcmp(data, tlsext_ri_serverhello, dlen) != 0) {
		FAIL("serverhello RI differs:\n");
		fprintf(stderr, "received:\n");
		hexdump(data, dlen);
		fprintf(stderr, "test data:\n");
		hexdump(tlsext_ri_serverhello, sizeof(tlsext_ri_serverhello));
		goto err;
	}

	CBS_init(&cbs, tlsext_ri_serverhello, sizeof(tlsext_ri_serverhello));
	if (!tlsext_ri_serverhello_parse(ssl, &cbs, &alert)) {
		FAIL("failed to parse serverhello RI\n");
		goto err;
	}

	if (S3I(ssl)->renegotiate_seen != 1) {
		FAIL("renegotiate seen not set\n");
		goto err;
	}
        if (S3I(ssl)->send_connection_binding != 1) {
		FAIL("send connection binding not set\n");
		goto err;
	}

	memset(S3I(ssl)->previous_client_finished, 0,
	    sizeof(S3I(ssl)->previous_client_finished));
	memset(S3I(ssl)->previous_server_finished, 0,
	    sizeof(S3I(ssl)->previous_server_finished));

	S3I(ssl)->renegotiate_seen = 0;

	CBS_init(&cbs, tlsext_ri_serverhello, sizeof(tlsext_ri_serverhello));
	if (tlsext_ri_serverhello_parse(ssl, &cbs, &alert)) {
		FAIL("parsed invalid serverhello RI\n");
		goto err;
	}

	if (S3I(ssl)->renegotiate_seen == 1) {
		FAIL("renegotiate seen set\n");
		goto err;
	}

	failure = 0;

 err:
	CBB_cleanup(&cbb);
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);
	free(data);

	return (failure);
}

/*
 * Signature Algorithms - RFC 5246 section 7.4.1.4.1.
 */

static unsigned char tlsext_sigalgs_clienthello[] = {
	0x00, 0x1a, 0x06, 0x01, 0x06, 0x03, 0xef, 0xef,
	0x05, 0x01, 0x05, 0x03, 0x04, 0x01, 0x04, 0x03,
	0xee, 0xee, 0xed, 0xed, 0x03, 0x01, 0x03, 0x03,
	0x02, 0x01, 0x02, 0x03,
};

static int
test_tlsext_sigalgs_clienthello(void)
{
	unsigned char *data = NULL;
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	int failure = 0;
	size_t dlen;
	int alert;
	CBB cbb;
	CBS cbs;

	CBB_init(&cbb, 0);

	if ((ssl_ctx = SSL_CTX_new(TLS_client_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	ssl->client_version = TLS1_1_VERSION;

	if (tlsext_sigalgs_clienthello_needs(ssl)) {
		fprintf(stderr, "FAIL: clienthello should not need sigalgs\n");
		failure = 1;
		goto done;
	}

	ssl->client_version = TLS1_2_VERSION;

	if (!tlsext_sigalgs_clienthello_needs(ssl)) {
		fprintf(stderr, "FAIL: clienthello should need sigalgs\n");
		failure = 1;
		goto done;
	}

	if (!tlsext_sigalgs_clienthello_build(ssl, &cbb)) {
		fprintf(stderr, "FAIL: clienthello failed to build sigalgs\n");
		failure = 1;
		goto done;
	}

	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	if (dlen != sizeof(tlsext_sigalgs_clienthello)) {
		fprintf(stderr, "FAIL: got clienthello sigalgs with length %zu, "
		    "want length %zu\n", dlen, sizeof(tlsext_sigalgs_clienthello));
		failure = 1;
		goto done;
	}

	if (memcmp(data, tlsext_sigalgs_clienthello, dlen) != 0) {
		fprintf(stderr, "FAIL: clienthello SNI differs:\n");
		fprintf(stderr, "received:\n");
		hexdump(data, dlen);
		fprintf(stderr, "test data:\n");
		hexdump(tlsext_sigalgs_clienthello, sizeof(tlsext_sigalgs_clienthello));
		failure = 1;
		goto done;
	}

	CBS_init(&cbs, tlsext_sigalgs_clienthello, sizeof(tlsext_sigalgs_clienthello));
	if (!tlsext_sigalgs_clienthello_parse(ssl, &cbs, &alert)) {
		fprintf(stderr, "FAIL: failed to parse clienthello SNI\n");
		failure = 1;
		goto done;
	}

	if (ssl->cert->pkeys[SSL_PKEY_RSA_SIGN].digest != EVP_sha512()) {
		fprintf(stderr, "FAIL: RSA sign digest mismatch\n");
		failure = 1;
		goto done;
	}
	if (ssl->cert->pkeys[SSL_PKEY_RSA_ENC].digest != EVP_sha512()) {
		fprintf(stderr, "FAIL: RSA enc digest mismatch\n");
		failure = 1;
		goto done;
	}
	if (ssl->cert->pkeys[SSL_PKEY_ECC].digest != EVP_sha512()) {
		fprintf(stderr, "FAIL: ECC digest mismatch\n");
		failure = 1;
		goto done;
	}
	if (ssl->cert->pkeys[SSL_PKEY_GOST01].digest != EVP_streebog512()) {
		fprintf(stderr, "FAIL: GOST01 digest mismatch\n");
		failure = 1;
		goto done;
	}
		
 done:
	CBB_cleanup(&cbb);
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);
	free(data);

	return (failure);
}

static int
test_tlsext_sigalgs_serverhello(void)
{
	unsigned char *data = NULL;
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	int failure = 0;
	size_t dlen;
	int alert;
	CBB cbb;
	CBS cbs;

	CBB_init(&cbb, 0);

	if ((ssl_ctx = SSL_CTX_new(TLS_server_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	if (tlsext_sigalgs_serverhello_needs(ssl)) {
		fprintf(stderr, "FAIL: serverhello should not need sigalgs\n");
		failure = 1;
		goto done;
	}

	if (tlsext_sigalgs_serverhello_build(ssl, &cbb)) {
		fprintf(stderr, "FAIL: serverhello should not build sigalgs\n");
		failure = 1;
		goto done;
	}

	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	CBS_init(&cbs, tlsext_sigalgs_clienthello, sizeof(tlsext_sigalgs_clienthello));
	if (tlsext_sigalgs_serverhello_parse(ssl, &cbs, &alert)) {
		fprintf(stderr, "FAIL: failed to parse serverhello sigalgs\n");
		failure = 1;
		goto done;
	}

 done:
	CBB_cleanup(&cbb);
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);
	free(data);

	return (failure);
}

/*
 * Server Name Indication - RFC 6066 section 3.
 */

#define TEST_SNI_SERVERNAME "www.libressl.org"

static unsigned char tlsext_sni_clienthello[] = {
	0x00, 0x13, 0x00, 0x00, 0x10, 0x77, 0x77, 0x77,
	0x2e, 0x6c, 0x69, 0x62, 0x72, 0x65, 0x73, 0x73,
	0x6c, 0x2e, 0x6f, 0x72, 0x67,
};

static unsigned char tlsext_sni_serverhello[] = {
};

static int
test_tlsext_sni_clienthello(void)
{
	unsigned char *data = NULL;
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	int failure;
	size_t dlen;
	int alert;
	CBB cbb;
	CBS cbs;

	failure = 1;

	CBB_init(&cbb, 0);

	if ((ssl_ctx = SSL_CTX_new(TLS_client_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	if (tlsext_sni_clienthello_needs(ssl)) {
		FAIL("clienthello should not need SNI\n");
		goto err;
	}

	if (!SSL_set_tlsext_host_name(ssl, TEST_SNI_SERVERNAME)) {
		FAIL("client failed to set server name\n");
		goto err;
	}

	if (!tlsext_sni_clienthello_needs(ssl)) {
		FAIL("clienthello should need SNI\n");
		goto err;
	}

	if (!tlsext_sni_clienthello_build(ssl, &cbb)) {
		FAIL("clienthello failed to build SNI\n");
		goto err;
	}

	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	if (dlen != sizeof(tlsext_sni_clienthello)) {
		FAIL("got clienthello SNI with length %zu, "
		    "want length %zu\n", dlen, sizeof(tlsext_sni_clienthello));
		goto err;
	}

	if (memcmp(data, tlsext_sni_clienthello, dlen) != 0) {
		FAIL("clienthello SNI differs:\n");
		fprintf(stderr, "received:\n");
		hexdump(data, dlen);
		fprintf(stderr, "test data:\n");
		hexdump(tlsext_sni_clienthello, sizeof(tlsext_sni_clienthello));
		goto err;
	}

	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	ssl->internal->hit = 0;

	CBS_init(&cbs, tlsext_sni_clienthello, sizeof(tlsext_sni_clienthello));
	if (!tlsext_sni_clienthello_parse(ssl, &cbs, &alert)) {
		FAIL("failed to parse clienthello SNI\n");
		goto err;
	}

	if (ssl->session->tlsext_hostname == NULL) {
		FAIL("no tlsext_hostname from clienthello SNI\n");
		goto err;
	}

	if (strlen(ssl->session->tlsext_hostname) != strlen(TEST_SNI_SERVERNAME) ||
	    strncmp(ssl->session->tlsext_hostname, TEST_SNI_SERVERNAME,
		strlen(TEST_SNI_SERVERNAME)) != 0) {
		FAIL("got tlsext_hostname `%s', want `%s'\n",
		    ssl->session->tlsext_hostname, TEST_SNI_SERVERNAME);
		goto err;
	}

	ssl->internal->hit = 1;

	if ((ssl->session->tlsext_hostname = strdup("notthesame.libressl.org")) ==
	    NULL)
		errx(1, "failed to strdup tlsext_hostname");

	CBS_init(&cbs, tlsext_sni_clienthello, sizeof(tlsext_sni_clienthello));
	if (tlsext_sni_clienthello_parse(ssl, &cbs, &alert)) {
		FAIL("parsed clienthello with mismatched SNI\n");
		goto err;
	}

	failure = 0;

 err:
	CBB_cleanup(&cbb);
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);
	free(data);

	return (failure);
}

static int
test_tlsext_sni_serverhello(void)
{
	unsigned char *data = NULL;
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	int failure;
	size_t dlen;
	int alert;
	CBB cbb;
	CBS cbs;

	failure = 1;

	CBB_init(&cbb, 0);

	if ((ssl_ctx = SSL_CTX_new(TLS_server_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	if (tlsext_sni_serverhello_needs(ssl)) {
		FAIL("serverhello should not need SNI\n");
		goto err;
	}

	if (!SSL_set_tlsext_host_name(ssl, TEST_SNI_SERVERNAME)) {
		FAIL("client failed to set server name\n");
		goto err;
	}

	if ((ssl->session->tlsext_hostname = strdup(TEST_SNI_SERVERNAME)) ==
	    NULL)
		errx(1, "failed to strdup tlsext_hostname");

	if (!tlsext_sni_serverhello_needs(ssl)) {
		FAIL("serverhello should need SNI\n");
		goto err;
	}

	if (!tlsext_sni_serverhello_build(ssl, &cbb)) {
		FAIL("serverhello failed to build SNI\n");
		goto err;
	}

	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	if (dlen != sizeof(tlsext_sni_serverhello)) {
		FAIL("got serverhello SNI with length %zu, "
		    "want length %zu\n", dlen, sizeof(tlsext_sni_serverhello));
		goto err;
	}

	if (memcmp(data, tlsext_sni_serverhello, dlen) != 0) {
		FAIL("serverhello SNI differs:\n");
		fprintf(stderr, "received:\n");
		hexdump(data, dlen);
		fprintf(stderr, "test data:\n");
		hexdump(tlsext_sni_serverhello, sizeof(tlsext_sni_serverhello));
		goto err;
	}

	free(ssl->session->tlsext_hostname);
	ssl->session->tlsext_hostname = NULL;

	CBS_init(&cbs, tlsext_sni_serverhello, sizeof(tlsext_sni_serverhello));
	if (!tlsext_sni_serverhello_parse(ssl, &cbs, &alert)) {
		FAIL("failed to parse serverhello SNI\n");
		goto err;
	}

	if (ssl->session->tlsext_hostname == NULL) {
		FAIL("no tlsext_hostname after serverhello SNI\n");
		goto err;
	}

	if (strlen(ssl->session->tlsext_hostname) != strlen(TEST_SNI_SERVERNAME) ||
	    strncmp(ssl->session->tlsext_hostname, TEST_SNI_SERVERNAME,
		strlen(TEST_SNI_SERVERNAME)) != 0) {
		FAIL("got tlsext_hostname `%s', want `%s'\n",
		    ssl->session->tlsext_hostname, TEST_SNI_SERVERNAME);
		goto err;
	}

	failure = 0;

 err:
	CBB_cleanup(&cbb);
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);
	free(data);

	return (failure);
}

static unsigned char tls_ocsp_clienthello_default[] = {
	0x01, 0x00, 0x00, 0x00, 0x00
};

static int
test_tlsext_ocsp_clienthello(void)
{
	unsigned char *data = NULL;
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	size_t dlen;
	int failure;
	int alert;
	CBB cbb;
	CBS cbs;

	failure = 1;

	CBB_init(&cbb, 0);

	if ((ssl_ctx = SSL_CTX_new(TLS_client_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	if (tlsext_ocsp_clienthello_needs(ssl)) {
		FAIL("clienthello should not need ocsp\n");
		goto err;
	}
	SSL_set_tlsext_status_type(ssl, TLSEXT_STATUSTYPE_ocsp);

	if (!tlsext_ocsp_clienthello_needs(ssl)) {
		FAIL("clienthello should need ocsp\n");
		goto err;
	}
	if (!tlsext_ocsp_clienthello_build(ssl, &cbb)) {
		FAIL("clienthello failed to build SNI\n");
		goto err;
	}
	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	if (dlen != sizeof(tls_ocsp_clienthello_default)) {
		FAIL("got ocsp clienthello with length %zu, "
		    "want length %zu\n", dlen,
		    sizeof(tls_ocsp_clienthello_default));
		goto err;
	}
	if (memcmp(data, tls_ocsp_clienthello_default, dlen) != 0) {
		FAIL("ocsp clienthello differs:\n");
		fprintf(stderr, "received:\n");
		hexdump(data, dlen);
		fprintf(stderr, "test data:\n");
		hexdump(tls_ocsp_clienthello_default,
		    sizeof(tls_ocsp_clienthello_default));
		goto err;
	}
	CBS_init(&cbs, tls_ocsp_clienthello_default,
	    sizeof(tls_ocsp_clienthello_default));
	if (!tlsext_ocsp_clienthello_parse(ssl, &cbs, &alert)) {
		FAIL("failed to parse ocsp clienthello\n");
		goto err;
	}

	failure = 0;

 err:
	CBB_cleanup(&cbb);
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);
	free(data);

	return (failure);
}

static int
test_tlsext_ocsp_serverhello(void)
{
	unsigned char *data = NULL;
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	size_t dlen;
	int failure;
	CBB cbb;

	failure = 1;

	CBB_init(&cbb, 0);

	if ((ssl_ctx = SSL_CTX_new(TLS_client_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	if (tlsext_ocsp_serverhello_needs(ssl)) {
		FAIL("serverhello should not need ocsp\n");
		goto err;
	}

	ssl->internal->tlsext_status_expected = 1;

	if (!tlsext_ocsp_serverhello_needs(ssl)) {
		FAIL("serverhello should need ocsp\n");
		goto err;
	}
	if (!tlsext_ocsp_serverhello_build(ssl, &cbb)) {
		FAIL("serverhello failed to build ocsp\n");
		goto err;
	}

	if (!CBB_finish(&cbb, &data, &dlen))
		errx(1, "failed to finish CBB");

	failure = 0;

 err:
	CBB_cleanup(&cbb);
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);
	free(data);

	return (failure);
}

/*
 * Session ticket - RFC 5077 since no known implementations use 4507.
 *
 * Session tickets can be length 0 (special case) to 2^16-1.
 *
 * The state is encrypted by the server so it is opaque to the client.
 */
static uint8_t tlsext_sessionticket_hello_min[1];
static uint8_t tlsext_sessionticket_hello_max[65535];

static int
test_tlsext_sessionticket_clienthello(void)
{
	unsigned char *data = NULL;
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	int failure;
	CBB cbb;
	size_t dlen;
	uint8_t dummy[1234];

	failure = 1;

	CBB_init(&cbb, 0);

	/* Create fake session tickets with random data. */
	arc4random_buf(tlsext_sessionticket_hello_min,
	    sizeof(tlsext_sessionticket_hello_min));
	arc4random_buf(tlsext_sessionticket_hello_max,
	    sizeof(tlsext_sessionticket_hello_max));

	if ((ssl_ctx = SSL_CTX_new(TLS_client_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	/* Should need a ticket by default. */
	if (!tlsext_sessionticket_clienthello_needs(ssl)) {
		FAIL("clienthello should need Sessionticket for default "
		    "ciphers\n");
		goto err;
	}

	/* Test disabling tickets. */
	if ((SSL_set_options(ssl, SSL_OP_NO_TICKET) & SSL_OP_NO_TICKET) == 0) {
		FAIL("Cannot disable tickets in the TLS connection");
		return 0;
	}
	if (tlsext_sessionticket_clienthello_needs(ssl)) {
		FAIL("clienthello should not need SessionTicket if it was disabled");
		goto err;
	}

	/* Test re-enabling tickets. */
	if ((SSL_clear_options(ssl, SSL_OP_NO_TICKET) & SSL_OP_NO_TICKET) != 0) {
		FAIL("Cannot re-enable tickets in the TLS connection");
		return 0;
	}
	if (!tlsext_sessionticket_clienthello_needs(ssl)) {
		FAIL("clienthello should need SessionTicket if it was disabled");
		goto err;
	}

	/* Since we don't have a session, we should build an empty ticket. */
	if (!tlsext_sessionticket_clienthello_build(ssl, &cbb)) {
		FAIL("Cannot build a ticket");
		goto err;
	}
	if (!CBB_finish(&cbb, &data, &dlen)) {
		FAIL("Cannot finish CBB");
		goto err;
	}
	if (dlen != 0) {
		FAIL("Expected 0 length but found %zu\n", dlen);
		goto err;
	}

	CBB_cleanup(&cbb);
	CBB_init(&cbb, 0);
	free(data);
	data = NULL;

	/* With a new session (but no ticket), we should still have 0 length */
	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");
	if (!tlsext_sessionticket_clienthello_needs(ssl)) {
		FAIL("Should still want a session ticket with a new session");
		goto err;
	}
	if (!tlsext_sessionticket_clienthello_build(ssl, &cbb)) {
		FAIL("Cannot build a ticket");
		goto err;
	}
	if (!CBB_finish(&cbb, &data, &dlen)) {
		FAIL("Cannot finish CBB");
		goto err;
	}
	if (dlen != 0) {
		FAIL("Expected 0 length but found %zu\n", dlen);
		goto err;
	}

	CBB_cleanup(&cbb);
	CBB_init(&cbb, 0);
	free(data);
	data = NULL;

	/* With a new session (and ticket), we should use that ticket */
	SSL_SESSION_free(ssl->session);
	if ((ssl->session = SSL_SESSION_new()) == NULL)
		errx(1, "failed to create session");

	arc4random_buf(&dummy, sizeof(dummy));
	if ((ssl->session->tlsext_tick = malloc(sizeof(dummy))) == NULL) {
		errx(1, "failed to malloc");
	}
	memcpy(ssl->session->tlsext_tick, dummy, sizeof(dummy));
	ssl->session->tlsext_ticklen = sizeof(dummy);

	if (!tlsext_sessionticket_clienthello_needs(ssl)) {
		FAIL("Should still want a session ticket with a new session");
		goto err;
	}
	if (!tlsext_sessionticket_clienthello_build(ssl, &cbb)) {
		FAIL("Cannot build a ticket");
		goto err;
	}
	if (!CBB_finish(&cbb, &data, &dlen)) {
		FAIL("Cannot finish CBB");
		goto err;
	}
	if (dlen != sizeof(dummy)) {
		FAIL("Expected %zu length but found %zu\n", sizeof(dummy), dlen);
		goto err;
	}
	if (memcmp(data, dummy, dlen) != 0) {
		FAIL("serverhello SNI differs:\n");
		compare_data(data, dlen,
		    dummy, sizeof(dummy));
		goto err;
	}

	CBB_cleanup(&cbb);
	CBB_init(&cbb, 0);
	free(data);
	data = NULL;
	free(ssl->session->tlsext_tick);
	ssl->session->tlsext_tick = NULL;
	ssl->session->tlsext_ticklen = 0;

	/*
	 * Send in NULL to disable session tickets at runtime without going
	 * through SSL_set_options().
	 */
	if (!SSL_set_session_ticket_ext(ssl, NULL, 0)) {
		FAIL("Could not set a NULL custom ticket");
		goto err;
	}
	/* Should not need a ticket in this case */
	if (tlsext_sessionticket_clienthello_needs(ssl)) {
		FAIL("Should not want to use session tickets with a NULL custom");
		goto err;
	}

	/*
	 * If you want to remove the tlsext_session_ticket behavior, you have
	 * to do it manually.
	 */
	free(ssl->internal->tlsext_session_ticket);
	ssl->internal->tlsext_session_ticket = NULL;

	if (!tlsext_sessionticket_clienthello_needs(ssl)) {
		FAIL("Should need a session ticket again when the custom one is removed");
		goto err;
	}

	/* Test a custom session ticket (not recommended in practice) */
	if (!SSL_set_session_ticket_ext(ssl, tlsext_sessionticket_hello_max,
	    sizeof(tlsext_sessionticket_hello_max))) {
		FAIL("Should be able to set a custom ticket");
		goto err;
	}
	if (!tlsext_sessionticket_clienthello_needs(ssl)) {
		FAIL("Should need a session ticket again when the custom one is not empty");
		goto err;
	}
	if (!tlsext_sessionticket_clienthello_build(ssl, &cbb)) {
		FAIL("Cannot build a ticket with a max length random payload");
		goto err;
	}
	if (!CBB_finish(&cbb, &data, &dlen)) {
		FAIL("Cannot finish CBB");
		goto err;
	}
	if (dlen != sizeof(tlsext_sessionticket_hello_max)) {
		FAIL("Expected %zu length but found %zu\n",
		    sizeof(tlsext_sessionticket_hello_max), dlen);
		goto err;
	}
	if (memcmp(data, tlsext_sessionticket_hello_max,
	    sizeof(tlsext_sessionticket_hello_max)) != 0) {
		FAIL("Expected to get what we passed in");
		compare_data(data, dlen,
		    tlsext_sessionticket_hello_max,
		    sizeof(tlsext_sessionticket_hello_max));
		goto err;
	}

	failure = 0;

 err:
	CBB_cleanup(&cbb);
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);
	free(data);

	return (failure);
}


static int
test_tlsext_sessionticket_serverhello(void)
{
	SSL_CTX *ssl_ctx = NULL;
	SSL *ssl = NULL;
	int failure;
	uint8_t *data;
	size_t dlen;
	CBB cbb;

	CBB_init(&cbb, 0);

	failure = 1;

	if ((ssl_ctx = SSL_CTX_new(TLS_server_method())) == NULL)
		errx(1, "failed to create SSL_CTX");
	if ((ssl = SSL_new(ssl_ctx)) == NULL)
		errx(1, "failed to create SSL");

	/*
	 * By default, should not need a session ticket since the ticket
	 * is not yet expected.
	 */
	if (tlsext_sessionticket_serverhello_needs(ssl)) {
		FAIL("serverhello should not need SessionTicket by default\n");
		goto err;
	}

	/* Test disabling tickets. */
	if ((SSL_set_options(ssl, SSL_OP_NO_TICKET) & SSL_OP_NO_TICKET) == 0) {
		FAIL("Cannot disable tickets in the TLS connection");
		return 0;
	}
	if (tlsext_sessionticket_serverhello_needs(ssl)) {
		FAIL("serverhello should not need SessionTicket if it was disabled");
		goto err;
	}

	/* Test re-enabling tickets. */
	if ((SSL_clear_options(ssl, SSL_OP_NO_TICKET) & SSL_OP_NO_TICKET) != 0) {
		FAIL("Cannot re-enable tickets in the TLS connection");
		return 0;
	}
	if (tlsext_sessionticket_serverhello_needs(ssl)) {
		FAIL("serverhello should not need SessionTicket yet");
		goto err;
	}

	/* Set expected to require it. */
	ssl->internal->tlsext_ticket_expected = 1;
	if (!tlsext_sessionticket_serverhello_needs(ssl)) {
		FAIL("serverhello should now be required for SessionTicket");
		goto err;
	}

	/* server hello's session ticket should always be 0 length payload. */
	if (!tlsext_sessionticket_serverhello_build(ssl, &cbb)) {
		FAIL("Cannot build a ticket with a max length random payload");
		goto err;
	}
	if (!CBB_finish(&cbb, &data, &dlen)) {
		FAIL("Cannot finish CBB");
		goto err;
	}
	if (dlen != 0) {
		FAIL("Expected 0 length but found %zu\n", dlen);
		goto err;
	}

	failure = 0;

 err:
	SSL_CTX_free(ssl_ctx);
	SSL_free(ssl);

	return (failure);
}

int
main(int argc, char **argv)
{
	int failed = 0;

	SSL_library_init();

	failed |= test_tlsext_ec_clienthello();
	failed |= test_tlsext_ec_serverhello();

	failed |= test_tlsext_ecpf_clienthello();
	failed |= test_tlsext_ecpf_serverhello();

	failed |= test_tlsext_ri_clienthello();
	failed |= test_tlsext_ri_serverhello();

	failed |= test_tlsext_sigalgs_clienthello();
	failed |= test_tlsext_sigalgs_serverhello();

	failed |= test_tlsext_sni_clienthello();
	failed |= test_tlsext_sni_serverhello();

	failed |= test_tlsext_ocsp_clienthello();
	failed |= test_tlsext_ocsp_serverhello();

	failed |= test_tlsext_sessionticket_clienthello();
	failed |= test_tlsext_sessionticket_serverhello();

	return (failed);
}
