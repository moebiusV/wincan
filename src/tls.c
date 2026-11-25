/*
 * tls.c -- candled's encrypted, key-authenticated connections (see tls.h).
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/rand.h>
#include <openssl/err.h>

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#define PEER_CERT(s) SSL_get1_peer_certificate(s)
#else
#define PEER_CERT(s) SSL_get_peer_certificate(s)
#endif

static void sslerr(const char *what)
{
    fprintf(stderr, "candled: %s\n", what);
    ERR_print_errors_fp(stderr);
}

/* ------------------------------------------------------------------ fingerprints */

int tls_norm_fp(const char *in, char out[TLS_FP_LEN])
{
    char hex[65]; size_t n = 0;
    const char *p = in;
    const char *f = strstr(p, "Fingerprint=");              /* what `openssl x509 -fingerprint` prints */
    if (f) p = f + strlen("Fingerprint=");
    while (*p == ' ' || *p == '\t') p++;
    if (!f && !strncasecmp(p, "sha256", 6)) { p += 6; if (*p == ':' || *p == '=') p++; }
    for (; *p; p++) {
        if (isxdigit((unsigned char)*p)) { if (n == 64) return -1; hex[n++] = (char)tolower((unsigned char)*p); }
        else if (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') continue;
        else return -1;
    }
    if (n != 64) return -1;
    hex[n] = 0;
    snprintf(out, TLS_FP_LEN, "SHA256:%s", hex);
    return 0;
}

static int x509_fp(X509 *x, char fp[TLS_FP_LEN])
{
    unsigned char md[EVP_MAX_MD_SIZE]; unsigned int len = 0;
    if (X509_digest(x, EVP_sha256(), md, &len) != 1 || len != 32) return -1;
    memcpy(fp, "SHA256:", 7);
    for (unsigned i = 0; i < 32; i++) snprintf(fp + 7 + 2 * i, 3, "%02x", md[i]);
    return 0;
}

int tls_cert_fingerprint(const char *certfile, char fp[TLS_FP_LEN])
{
    FILE *f = fopen(certfile, "r");
    X509 *x;
    int r;
    if (!f) { fprintf(stderr, "candled: %s: %s\n", certfile, strerror(errno)); return -1; }
    x = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    if (!x) { sslerr("cannot read the certificate"); return -1; }
    r = x509_fp(x, fp);
    X509_free(x);
    return r;
}

/* ------------------------------------------------------------------ the server's own certificate */

static void mkparent(const char *path)                          /* the directory of a file, one level */
{
    char d[1024]; char *s;
    snprintf(d, sizeof d, "%s", path);
    s = strrchr(d, '/');
    if (s && s != d) { *s = 0; mkdir(d, 0755); }
}

static FILE *create_file(const char *path, mode_t mode, int force)
{
    int fd = open(path, O_WRONLY | O_CREAT | (force ? O_TRUNC : O_EXCL), mode);
    if (fd < 0) return NULL;
    fchmod(fd, mode);
    return fdopen(fd, "w");
}

int tls_gen_cert(const char *certfile, const char *keyfile, int force, char fp[TLS_FP_LEN])
{
    EVP_PKEY *pk = NULL; EVP_PKEY_CTX *kc = NULL; X509 *x = NULL; X509_NAME *nm; FILE *f;
    char host[256] = "candled"; unsigned char rnd[8]; int rc = -1;
    if (!force && access(keyfile, F_OK) == 0 && access(certfile, F_OK) == 0) {
        fprintf(stderr, "candled: %s and %s exist (use --force to replace them)\n", certfile, keyfile);
        return tls_cert_fingerprint(certfile, fp) == 0 ? 0 : -1;
    }
    gethostname(host, sizeof host - 1); host[sizeof host - 1] = 0;
    kc = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!kc || EVP_PKEY_keygen_init(kc) <= 0 || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kc, NID_X9_62_prime256v1) <= 0 || EVP_PKEY_keygen(kc, &pk) <= 0) { sslerr("cannot make a key"); goto out; }
    x = X509_new();
    if (!x) goto out;
    X509_set_version(x, 2);
    RAND_bytes(rnd, sizeof rnd); rnd[0] &= 0x7f;
    { ASN1_INTEGER *sn = X509_get_serialNumber(x); BIGNUM *bn = BN_bin2bn(rnd, sizeof rnd, NULL); BN_to_ASN1_INTEGER(bn, sn); BN_free(bn); }
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x), 3650L * 86400L);
    X509_set_pubkey(x, pk);
    nm = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC, (const unsigned char *)host, -1, -1, 0);
    X509_set_issuer_name(x, nm);
    {   /* usable as a server certificate by clients that check the usage */
        X509V3_CTX c; X509_EXTENSION *e;
        X509V3_set_ctx_nodb(&c); X509V3_set_ctx(&c, x, x, NULL, NULL, 0);
        if ((e = X509V3_EXT_conf_nid(NULL, &c, NID_basic_constraints, "CA:FALSE"))) { X509_add_ext(x, e, -1); X509_EXTENSION_free(e); }
        if ((e = X509V3_EXT_conf_nid(NULL, &c, NID_key_usage, "digitalSignature"))) { X509_add_ext(x, e, -1); X509_EXTENSION_free(e); }
        if ((e = X509V3_EXT_conf_nid(NULL, &c, NID_ext_key_usage, "serverAuth"))) { X509_add_ext(x, e, -1); X509_EXTENSION_free(e); }
    }
    if (!X509_sign(x, pk, EVP_sha256())) { sslerr("cannot sign the certificate"); goto out; }
    mkparent(keyfile); mkparent(certfile);
    if (!(f = create_file(keyfile, 0600, force))) { fprintf(stderr, "candled: %s: %s\n", keyfile, strerror(errno)); goto out; }
    if (!PEM_write_PrivateKey(f, pk, NULL, NULL, 0, NULL, NULL)) { sslerr("cannot write the key"); fclose(f); goto out; }
    if (fclose(f) != 0) { fprintf(stderr, "candled: %s: %s\n", keyfile, strerror(errno)); goto out; }
    if (!(f = create_file(certfile, 0644, force))) { fprintf(stderr, "candled: %s: %s\n", certfile, strerror(errno)); goto out; }
    if (!PEM_write_X509(f, x)) { sslerr("cannot write the certificate"); fclose(f); goto out; }
    if (fclose(f) != 0) { fprintf(stderr, "candled: %s: %s\n", certfile, strerror(errno)); goto out; }
    rc = x509_fp(x, fp);
out:
    if (x) X509_free(x);
    if (pk) EVP_PKEY_free(pk);
    if (kc) EVP_PKEY_CTX_free(kc);
    return rc;
}

/* ------------------------------------------------------------------ the authorized clients */

typedef char Fp[TLS_FP_LEN];
static pthread_mutex_t az_mu = PTHREAD_MUTEX_INITIALIZER;
static char *az_file;
static Fp *az_inl; static size_t az_ninl;
static Fp *az_list; static size_t az_n, az_cap;
static time_t az_mtime; static off_t az_size; static int az_loaded;

void tls_authz_set(const char *file, char **inline_fps, size_t n)
{
    pthread_mutex_lock(&az_mu);
    free(az_file); az_file = file ? strdup(file) : NULL;
    free(az_inl); az_inl = NULL; az_ninl = 0;
    if (n && (az_inl = malloc(n * sizeof *az_inl))) {
        for (size_t i = 0; i < n; i++) if (tls_norm_fp(inline_fps[i], az_inl[az_ninl]) == 0) az_ninl++;
    }
    az_loaded = 0;
    pthread_mutex_unlock(&az_mu);
}

/* read the file again if it has changed (under az_mu) */
static void az_reload(void)
{
    struct stat st; FILE *f; char line[512];
    if (!az_file) return;
    if (stat(az_file, &st) != 0) { az_n = 0; az_loaded = 1; az_mtime = 0; az_size = 0; return; }
    if (az_loaded && st.st_mtime == az_mtime && st.st_size == az_size) return;
    az_n = 0; az_mtime = st.st_mtime; az_size = st.st_size; az_loaded = 1;
    if (!(f = fopen(az_file, "r"))) return;
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'), *t;
        Fp fp;
        if (h) *h = 0;
        t = line + strspn(line, " \t");
        { char *e = t; while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++; *e = 0; }
        if (!*t || tls_norm_fp(t, fp) != 0) continue;
        if (az_n == az_cap) { size_t nc = az_cap ? az_cap * 2 : 16; Fp *nl = realloc(az_list, nc * sizeof *nl); if (!nl) break; az_list = nl; az_cap = nc; }
        memcpy(az_list[az_n++], fp, sizeof fp);
    }
    fclose(f);
}

int tls_authz_check(const char *fp)
{
    int ok = 0;
    pthread_mutex_lock(&az_mu);
    az_reload();
    for (size_t i = 0; i < az_ninl && !ok; i++) ok = strcmp(az_inl[i], fp) == 0;
    for (size_t i = 0; i < az_n && !ok; i++) ok = strcmp(az_list[i], fp) == 0;
    pthread_mutex_unlock(&az_mu);
    return ok;
}

int tls_authz_add(const char *file, const char *fp_in, const char *comment)
{
    Fp fp; FILE *f; char line[512]; time_t now = time(NULL); struct tm tm;
    char date[32];
    if (tls_norm_fp(fp_in, fp) != 0) { fprintf(stderr, "candled: not a SHA-256 fingerprint: %s\n", fp_in); return -1; }
    if ((f = fopen(file, "r"))) {                                 /* already there? */
        while (fgets(line, sizeof line, f)) {
            char *h = strchr(line, '#'), *t; Fp g;
            if (h) *h = 0;
            t = line + strspn(line, " \t");
            { char *e = t; while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++; *e = 0; }
            if (*t && tls_norm_fp(t, g) == 0 && strcmp(g, fp) == 0) { fclose(f); return 1; }
        }
        fclose(f);
    }
    mkparent(file);
    { int fd = open(file, O_WRONLY | O_CREAT | O_APPEND, 0640); if (fd < 0 || !(f = fdopen(fd, "a"))) { fprintf(stderr, "candled: %s: %s\n", file, strerror(errno)); return -1; } }
    gmtime_r(&now, &tm); strftime(date, sizeof date, "%Y-%m-%d", &tm);
    if (comment && *comment) fprintf(f, "%s  # %s (added %s)\n", fp, comment, date); else fprintf(f, "%s  # added %s\n", fp, date);
    if (fclose(f) != 0) { fprintf(stderr, "candled: %s: %s\n", file, strerror(errno)); return -1; }
    return 0;
}

/* ------------------------------------------------------------------ the handshake */

static SSL_CTX *ctx;

/* the client's certificate is self-signed, so the usual check cannot pass; it is accepted here and
 * allowed or not by its fingerprint afterwards (tls_authz_check) */
static int accept_any(int ok, X509_STORE_CTX *s) { (void)ok; (void)s; return 1; }

int tls_server_init(const char *certfile, const char *keyfile)
{
    OPENSSL_init_ssl(0, NULL);
    ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { sslerr("cannot start TLS"); return -1; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET | SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    if (SSL_CTX_use_certificate_chain_file(ctx, certfile) != 1) { fprintf(stderr, "candled: %s: ", certfile); sslerr("cannot use the certificate"); return -1; }
    if (SSL_CTX_use_PrivateKey_file(ctx, keyfile, SSL_FILETYPE_PEM) != 1) { fprintf(stderr, "candled: %s: ", keyfile); sslerr("cannot use the key"); return -1; }
    if (SSL_CTX_check_private_key(ctx) != 1) { sslerr("the key does not belong to the certificate"); return -1; }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, accept_any);
    return 0;
}

SSL *tls_accept(int fd, char fp[TLS_FP_LEN], const char **why)
{
    SSL *s = SSL_new(ctx);
    X509 *x;
    if (!s) { *why = "out of memory"; return NULL; }
    SSL_set_fd(s, fd);
    if (SSL_accept(s) != 1) { *why = "no TLS handshake (a plain client, or no client certificate)"; ERR_clear_error(); SSL_free(s); return NULL; }
    x = PEER_CERT(s);
    if (!x) { *why = "no client certificate"; SSL_free(s); return NULL; }
    if (x509_fp(x, fp) != 0) { *why = "cannot fingerprint the client certificate"; X509_free(x); SSL_free(s); return NULL; }
    X509_free(x);
    return s;
}
