/*
 * tls.h -- candled's encrypted, key-authenticated connections.
 *
 * Like an IRC bouncer that takes a client's certificate fingerprint (soju's certfp, SASL EXTERNAL):
 * the client makes its own key and certificate, and the server is told the SHA-256 fingerprint of the
 * certificate, which it then lets in.  The server's own certificate is self-signed; the client pins its
 * fingerprint.  A fingerprint is written "SHA256:" and 64 lower-case hexadecimal digits.
 */
#ifndef TLS_H
#define TLS_H

#include <stddef.h>
#include <openssl/ssl.h>

#define TLS_FP_LEN 72                  /* "SHA256:" + 64 digits + NUL */

/* Normalize a fingerprint as people write it ("SHA256:AB:CD:...", "ab cd ...", 64 bare digits) to the
 * canonical form.  Returns 0, or -1 if it is not a SHA-256 fingerprint. */
int tls_norm_fp(const char *in, char out[TLS_FP_LEN]);

/* Make a self-signed EC P-256 certificate (valid ten years) and its private key (mode 0600), and
 * return the certificate's fingerprint.  Existing files are kept unless force.  0, or -1 with a
 * message on standard error. */
int tls_gen_cert(const char *certfile, const char *keyfile, int force, char fp[TLS_FP_LEN]);

/* The fingerprint of the certificate in a PEM file. */
int tls_cert_fingerprint(const char *certfile, char fp[TLS_FP_LEN]);

/* The authorized clients: the fingerprints of the file (re-read when it changes) and the given list. */
void tls_authz_set(const char *file, char **inline_fps, size_t n);
int  tls_authz_check(const char *fp);                          /* 1 if the fingerprint is allowed */
/* Add a fingerprint (with a comment) to the file, creating it (mode 0640) and its directory if need
 * be; 0 if added, 1 if it was there, -1 on an error. */
int  tls_authz_add(const char *file, const char *fp, const char *comment);

/* The server side.  tls_server_init loads the certificate and key; tls_accept does the handshake on a
 * connected socket and returns the connection with the client's fingerprint, or NULL (the reason is
 * logged by the caller from *why).  The client's certificate is required; whether it is allowed is
 * for tls_authz_check. */
int  tls_server_init(const char *certfile, const char *keyfile);
SSL *tls_accept(int fd, char fp[TLS_FP_LEN], const char **why);

#endif /* TLS_H */
