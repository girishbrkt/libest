/*------------------------------------------------------------------
 * st_proxy.c - Simple single-threaded EST proxy server that can be
 *               used for testing.  This server can only handle
 *               a single EST request at any given time.  Attempting
 *               to send multiple requests to it will result in
 *               undetermined results.
 *
 * October, 2013
 *
 * Copyright (c) 2013 by cisco Systems, Inc.
 * All rights reserved.
 *------------------------------------------------------------------
 */
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <search.h>
#include <openssl/err.h>
#include <openssl/engine.h>
#include <openssl/conf.h>
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <est.h>
#include "ossl_srv.h"
#include "test_utils.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>


static int tcp_port;
volatile int stop_proxy_flag = 0;
unsigned char *proxy_cacerts_raw = NULL;
int proxy_cacerts_len = 0;
EST_CTX *epctx;
unsigned char *proxy_trustcerts = NULL;
int proxy_trustcerts_len = 0;

extern void dumpbin(char *buf, size_t len);

/*
 * We hard-code the DH parameters here.  THIS SHOULD NOT
 * be done in a real application.  The DH parameters need
 * to be generated at the time of product installation so
 * that every instance of the product in the field has
 * unique parameters.  Otherwise DH key exchange would be
 * vulnerable to attack.
 * See the OpenSSL documentation on generating DH parameters
 * for more information.
 */
static DH *get_dh1024dsa()
{
    static unsigned char dh1024_p[]={
	0xC8,0x00,0xF7,0x08,0x07,0x89,0x4D,0x90,0x53,0xF3,0xD5,0x00,
	0x21,0x1B,0xF7,0x31,0xA6,0xA2,0xDA,0x23,0x9A,0xC7,0x87,0x19,
	0x3B,0x47,0xB6,0x8C,0x04,0x6F,0xFF,0xC6,0x9B,0xB8,0x65,0xD2,
	0xC2,0x5F,0x31,0x83,0x4A,0xA7,0x5F,0x2F,0x88,0x38,0xB6,0x55,
	0xCF,0xD9,0x87,0x6D,0x6F,0x9F,0xDA,0xAC,0xA6,0x48,0xAF,0xFC,
	0x33,0x84,0x37,0x5B,0x82,0x4A,0x31,0x5D,0xE7,0xBD,0x52,0x97,
	0xA1,0x77,0xBF,0x10,0x9E,0x37,0xEA,0x64,0xFA,0xCA,0x28,0x8D,
	0x9D,0x3B,0xD2,0x6E,0x09,0x5C,0x68,0xC7,0x45,0x90,0xFD,0xBB,
	0x70,0xC9,0x3A,0xBB,0xDF,0xD4,0x21,0x0F,0xC4,0x6A,0x3C,0xF6,
	0x61,0xCF,0x3F,0xD6,0x13,0xF1,0x5F,0xBC,0xCF,0xBC,0x26,0x9E,
	0xBC,0x0B,0xBD,0xAB,0x5D,0xC9,0x54,0x39,
	};
    static unsigned char dh1024_g[]={
	0x3B,0x40,0x86,0xE7,0xF3,0x6C,0xDE,0x67,0x1C,0xCC,0x80,0x05,
	0x5A,0xDF,0xFE,0xBD,0x20,0x27,0x74,0x6C,0x24,0xC9,0x03,0xF3,
	0xE1,0x8D,0xC3,0x7D,0x98,0x27,0x40,0x08,0xB8,0x8C,0x6A,0xE9,
	0xBB,0x1A,0x3A,0xD6,0x86,0x83,0x5E,0x72,0x41,0xCE,0x85,0x3C,
	0xD2,0xB3,0xFC,0x13,0xCE,0x37,0x81,0x9E,0x4C,0x1C,0x7B,0x65,
	0xD3,0xE6,0xA6,0x00,0xF5,0x5A,0x95,0x43,0x5E,0x81,0xCF,0x60,
	0xA2,0x23,0xFC,0x36,0xA7,0x5D,0x7A,0x4C,0x06,0x91,0x6E,0xF6,
	0x57,0xEE,0x36,0xCB,0x06,0xEA,0xF5,0x3D,0x95,0x49,0xCB,0xA7,
	0xDD,0x81,0xDF,0x80,0x09,0x4A,0x97,0x4D,0xA8,0x22,0x72,0xA1,
	0x7F,0xC4,0x70,0x56,0x70,0xE8,0x20,0x10,0x18,0x8F,0x2E,0x60,
	0x07,0xE7,0x68,0x1A,0x82,0x5D,0x32,0xA2,
	};
    DH *dh;

    if ((dh=DH_new()) == NULL) {
	return(NULL);
    }
    dh->p=BN_bin2bn(dh1024_p,sizeof(dh1024_p),NULL);
    dh->g=BN_bin2bn(dh1024_g,sizeof(dh1024_g),NULL);
    if ((dh->p == NULL) || (dh->g == NULL)) { 
	DH_free(dh); return(NULL); 
    }
    dh->length = 160;
    return(dh);
}


/****************************************************************************************
 * The following funcitons are the callbacks used by libest.a to bind
 * the EST stack to the HTTP/SSL layer and the CA server.
 ***************************************************************************************/
static char digest_user[3][32] = 
    {
	"estuser", 
	"estrealm", 
	"36807fa200741bb0e8fb04fcf08e2de6" //This is the HA1 precaculated value
    };

/*
 * Return 1 to signal the user is valid, 0 to fail the auth
 */
static int process_http_auth (EST_CTX *ctx, EST_HTTP_AUTH_HDR *ah, 
	                      X509 *peer_cert, void *app_data)
{
    int user_valid = 0; 
    char *digest;

    switch (ah->mode) {
    case AUTH_BASIC:
	/*
	 * this is where we might hook into a Radius server
	 * or some external database to authenticate a 
	 * userID/password.  But for this example code,
	 * we just hard-code a local user for testing
	 * the libest API.
	 */
	if (!strcmp(ah->user, "estuser") && !strcmp(ah->pwd, "estpwd")) {
	    /* The user is valid */
	    user_valid = 1;
	} 
	break;
    case AUTH_DIGEST:
      /*
       * Check the user's name
       */
      if (strcmp(ah->user, digest_user[0])) {
	return 0;
      }
      
      /*
       * Call the helper function to calculate the digest,
       * This is then compared against the digest provided
       * by the client.
       */
      digest = est_server_generate_auth_digest(ah, digest_user[2]);
      if (!strncmp(digest, ah->response, 32)) {
	user_valid = 1;
	free(digest);
      }
      break;
    case AUTH_FAIL:
    case AUTH_NONE:
    default:
	return 0;
	break;
    }
    return user_valid;
}

static void cleanup() 
{
    est_server_stop(epctx);
    est_destroy(epctx);
    free(proxy_cacerts_raw);
    free(proxy_trustcerts);

    //We don't shutdown here because there
    //may be other unit test cases in this process
    //the occur later.
    //est_apps_shutdown();
}

static void* master_thread (void *arg)
{
    int sock;                 
    struct sockaddr_in6 addr;
    int on = 1;
    int rc;
    int flags;
    int new;
    int unsigned len;

    memset(&addr, 0x0, sizeof(struct sockaddr_in6));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons((uint16_t)tcp_port);

    sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (sock == -1) {
        fprintf(stderr, "\nsocket call failed\n");
        exit(1);
    }
    // Needs to be done to bind to both :: and 0.0.0.0 to the same port
    int no = 0;
    setsockopt(sock, SOL_SOCKET, IPV6_V6ONLY, (void *)&no, sizeof(no));

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char*)&on, sizeof(on));
    flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    rc = bind(sock, (const struct sockaddr*)&addr, sizeof(addr));
    if (rc == -1) {
        fprintf(stderr, "\nbind call failed\n");
        exit(1);
    }
    listen(sock, SOMAXCONN);
    stop_proxy_flag = 0;

    while (stop_proxy_flag == 0) {
        len = sizeof(addr);
        new = accept(sock, (struct sockaddr*)&addr, &len);
        if (new < 0) {
	    /*
	     * this is a bit cheesy, but much easier to implement than using select()
	     */
            usleep(100);
        } else {
            if (stop_proxy_flag == 0) {
		est_server_handle_request(epctx, new);
		close(new);
            }
        }
    }
    close(sock);
    cleanup();
    return NULL;
}


/*
 * Call this function to stop the single-threaded simple EST proxy server
 */
void st_proxy_stop ()
{
    stop_proxy_flag = 1;
    sleep(2);
}

/*
 * Call this to start a simple EST proxy server.  This server will not
 * be thread safe.  It can only handle a single EST request on
 * the listening socket at any given time.  This server will run
 * until st_proxy_stop() is invoked.
 *
 * Parameters:
 *  listen_port:    Port number to listen on
 *  certfile:	    PEM encoded certificate used for server's identity
 *  keyfile:	    Private key associated with the certfile
 *  realm:	    HTTP realm to present to the client
 *  ca_chain_file:  PEM encoded certificates to use in the /cacerts
 *                  response to the client. 
 *  trusted_certs_file: PEM encoded certificates to use for authenticating
 *                  the EST client at the TLS layer. 
 *  userid          User ID used by proxy to identify itself to the server for
 *                  HTTP authentication.
 *  password        The password associated with userid.
 *  server          Hostname or IP address of the CA EST server that this
 *                  proxy will forward requests too.
 *  server_port     TCP port number used by the CA EST server.
 *  ec_nid:         Openssl NID value for ECDHE curve to use during
 *                  TLS handshake.  Take values from <openssl/obj_mac.h>
 */
int st_proxy_start (int listen_port,
	            char *certfile,
	            char *keyfile,
	            char *realm,
	            char *ca_chain_file,
	            char *trusted_certs_file,
		    char *userid,
		    char *password,
		    char *server,
		    int server_port,
	            int enable_pop,
	            int ec_nid)
{
    X509 *x;
    EVP_PKEY *priv_key;
    BIO *certin, *keyin;
    DH *dh;
    pthread_t thread;
    EST_ERROR rv;

    /*
     * Read in the CA certificates
     * This is the explicit chain
     */
    if (ca_chain_file) {        
        proxy_cacerts_len = read_binary_file(ca_chain_file, &proxy_cacerts_raw);
        if (proxy_cacerts_len <= 0) {
            printf("\nCA chain file %s file could not be read\n", ca_chain_file);
            return (-1);
        }
    } else {
        proxy_cacerts_raw = NULL;
        proxy_cacerts_len = 0;
    }

    /*
     * Read in the external CA certificates
     * This is the implicit chain
     */
    if (trusted_certs_file) {
        proxy_trustcerts_len = read_binary_file(trusted_certs_file, &proxy_trustcerts);
        if (proxy_trustcerts_len <= 0) {
            printf("\nTrusted certs file %s could not be read\n", 
		    trusted_certs_file);
            return (-1);
        }
    }

    /*
     * Read in the local server certificate 
     */
    certin = BIO_new(BIO_s_file_internal());
    if (BIO_read_filename(certin, certfile) <= 0) {
	printf("\nUnable to read server certificate file %s\n", certfile);
	return (-1);
    }
    /*
     * This reads the file, which is expected to be PEM encoded.  If you're using 
     * DER encoded certs, you would invoke d2i_X509_bio() instead.
     */
    x = PEM_read_bio_X509(certin, NULL, NULL, NULL);
    if (x == NULL) {
	printf("\nError while reading PEM encoded server certificate file %s\n", certfile);
	return (-1);
    }
    BIO_free(certin);


    /* 
     * Read in the server's private key
     */
    keyin = BIO_new(BIO_s_file_internal());
    if (BIO_read_filename(keyin, keyfile) <= 0) {
	printf("\nUnable to read server private key file %s\n", keyfile);
	return (-1);
    }
    /*
     * This reads in the private key file, which is expected to be a PEM
     * encoded private key.  If using DER encoding, you would invoke
     * d2i_PrivateKey_bio() instead. 
     */
    priv_key = PEM_read_bio_PrivateKey(keyin, NULL, NULL, NULL);
    if (priv_key == NULL) {
	printf("\nError while reading PEM encoded private key file %s\n", certfile);
	return (-1);
    }
    BIO_free(keyin);

    //We don't startup here, it's expected the test harness
    //will call this.
    //est_apps_startup();

    est_init_logger(EST_LOG_LVL_INFO, NULL);
    epctx = est_proxy_init(proxy_trustcerts, proxy_trustcerts_len, 
                           proxy_cacerts_raw, proxy_cacerts_len,
	                   EST_CERT_FORMAT_PEM, realm, x, priv_key,
			   userid, password);
    if (!epctx) {
        printf("\nUnable to initialize EST context.  Aborting!!!\n");
        return (-1);
    }

    if (ec_nid) {
	est_server_set_ecdhe_curve(epctx, ec_nid);
    }

    if (!enable_pop) {
	est_server_disable_pop(epctx);
    }

    if (est_set_http_auth_cb(epctx, &process_http_auth)) {
        printf("\nUnable to set EST HTTP AUTH callback.  Aborting!!!\n");
        return (-1);
    }    

    /*
     * Specify the address of the CA EST server 
     */
    est_proxy_set_server(epctx, server, server_port);

    /*
     * Change the retry period to some value other
     * than the default so that it's different than
     * what the server is using.
     */
    est_server_set_retry_period(epctx, 19273);

    /*
     * Set DH parameters for TLS
     */
    dh = get_dh1024dsa();
    if (dh) {
	est_server_set_dh_parms(epctx, dh);
    }
    DH_free(dh);

    est_enable_crl(epctx);

    printf("\nLaunching EST proxy server...\n");

    rv = est_proxy_start(epctx);
    if (rv != EST_ERR_NONE) {
        printf("\nFailed to init mg\n");
        return (-1);
    }

    // Start master (listening) thread
    tcp_port = listen_port;
    pthread_create(&thread, NULL, master_thread, NULL);

    /* Clean up */
    EVP_PKEY_free(priv_key);
    X509_free(x);
    sleep(2);
    return 0;
}

void st_proxy_enable_pop ()
{
    est_server_enable_pop(epctx);
}

void st_proxy_disable_pop ()
{
    est_server_disable_pop(epctx);
}

void st_proxy_set_auth (EST_HTTP_AUTH_MODE auth_mode)
{
    est_proxy_set_auth_mode(epctx, auth_mode);
}

void st_proxy_set_read_timeout (int timeout)
{
    est_proxy_set_read_timeout(epctx, timeout);
}

void st_proxy_disable_http_auth ()
{
    est_set_http_auth_cb(epctx, NULL);
}

int st_proxy_http_disable (int disable)
{
    if (disable) {
        if (est_set_http_auth_cb(epctx, NULL)) {
            printf("\nUnable to set EST HTTP AUTH callback.  Aborting!!!\n");
            return (-1);
        }
    } else {
        if (est_set_http_auth_cb(epctx, &process_http_auth)) {
            printf("\nUnable to set EST HTTP AUTH callback.  Aborting!!!\n");
            return (-1);
        }
    }
    return (0);
}
