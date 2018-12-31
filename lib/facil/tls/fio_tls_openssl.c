/*
Copyright: Boaz Segev, 2018
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/
#include <fio.h>

/**
 * This implementation of the facil.io SSL/TLS wrapper API wraps the OpenSSL API
 * to provide TLS 1.2 and TLS 1.3 to facil.io applications.
 *
 * The implementation requires `HAVE_OPENSSL` to be set.
 */
#include "fio_tls.h"

#if HAVE_OPENSSL
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#define REQUIRE_LIBRARY()
#define FIO_TLS_WEAK

/* *****************************************************************************
The SSL/TLS helper data types
***************************************************************************** */
#define FIO_INCLUDE_STR 1
#define FIO_FORCE_MALLOC_TMP 1
#include <fio.h>

typedef struct {
  fio_str_s name; /* fio_str_s provides cache locality for small strings */
  void (*callback)(intptr_t uuid, void *udata);
} alpn_s;

static inline int fio_alpn_cmp(const alpn_s *dest, const alpn_s *src) {
  return fio_str_iseq(&dest->name, &src->name);
}
static inline void fio_alpn_copy(alpn_s *dest, alpn_s *src) {
  *dest = (alpn_s){.name = FIO_STR_INIT, .callback = src->callback};
  fio_str_concat(&dest->name, &src->name);
}
static inline void fio_alpn_destroy(alpn_s *obj) { fio_str_free(&obj->name); }

#define FIO_ARY_NAME alpn_ary
#define FIO_ARY_TYPE alpn_s
#define FIO_ARY_COMPARE(k1, k2) fio_alpn_cmp(&(k1), &(k2))
#define FIO_ARY_COPY(dest, obj) fio_alpn_copy(&(dest), &(obj))
#define FIO_ARY_DESTROY(key) fio_alpn_destroy(&(key))
#define FIO_FORCE_MALLOC_TMP 1
#include <fio.h>

typedef struct {
  fio_str_s private_key;
  fio_str_s public_key;
  fio_str_s password;
} cert_s;

static inline int fio_tls_cert_cmp(const cert_s *dest, const cert_s *src) {
  return fio_str_iseq(&dest->private_key, &src->private_key);
}
static inline void fio_tls_cert_copy(cert_s *dest, cert_s *src) {
  *dest = (cert_s){
      .private_key = FIO_STR_INIT,
      .public_key = FIO_STR_INIT,
      .password = FIO_STR_INIT,
  };
  fio_str_concat(&dest->private_key, &src->private_key);
  fio_str_concat(&dest->public_key, &src->public_key);
  fio_str_concat(&dest->password, &src->password);
}
static inline void fio_tls_cert_destroy(cert_s *obj) {
  fio_str_free(&obj->private_key);
  fio_str_free(&obj->public_key);
  fio_str_free(&obj->password);
}

#define FIO_ARY_NAME cert_ary
#define FIO_ARY_TYPE cert_s
#define FIO_ARY_COMPARE(k1, k2) (fio_tls_cert_cmp(&(k1), &(k2)))
#define FIO_ARY_COPY(dest, obj) fio_tls_cert_copy(&(dest), &(obj))
#define FIO_ARY_DESTROY(key) fio_tls_cert_destroy(&(key))
#define FIO_FORCE_MALLOC_TMP 1
#include <fio.h>

typedef struct {
  fio_str_s pem;
} trust_s;

static inline int fio_tls_trust_cmp(const trust_s *dest, const trust_s *src) {
  return fio_str_iseq(&dest->pem, &src->pem);
}
static inline void fio_tls_trust_copy(trust_s *dest, trust_s *src) {
  *dest = (trust_s){
      .pem = FIO_STR_INIT,
  };
  fio_str_concat(&dest->pem, &src->pem);
}
static inline void fio_tls_trust_destroy(trust_s *obj) {
  fio_str_free(&obj->pem);
}

#define FIO_ARY_NAME trust_ary
#define FIO_ARY_TYPE trust_s
#define FIO_ARY_COMPARE(k1, k2) (fio_tls_trust_cmp(&(k1), &(k2)))
#define FIO_ARY_COPY(dest, obj) fio_tls_trust_copy(&(dest), &(obj))
#define FIO_ARY_DESTROY(key) fio_tls_trust_destroy(&(key))
#define FIO_FORCE_MALLOC_TMP 1
#include <fio.h>

/* *****************************************************************************
The SSL/TLS type
***************************************************************************** */

/** An opaque type used for the SSL/TLS functions. */
struct fio_tls_s {
  alpn_ary_s alpn;   /* ALPN is the name for the protocol selection extension */
  cert_ary_s sni;    /* SNI is the name for the server name extension */
  trust_ary_s trust; /* SNI is the name for the server name extension */

  /************ TODO: implementation data fields go here ******************/

  SSL_CTX *ctx;            /* The Open SSL context (updated each time). */
  unsigned char *alpn_str; /* the computed server-format ALPN string */
  int alpn_len;
};

/* *****************************************************************************
OpenSSL Helpers
***************************************************************************** */

static EVP_PKEY *fio_tls_pkey = NULL;

static void fio_tls_clear_root_key(void *key) {
  EVP_PKEY_free(key);
  fio_tls_pkey = NULL;
}

static void fio_tls_make_root_key(void) {
  static fio_lock_i lock = FIO_LOCK_INIT;
  fio_lock(&lock);
  if (fio_tls_pkey)
    goto finish;
  /* create private key, free it at exit */
  FIO_LOG_DEBUG("calculating a new TLS private key... might take a while.");

  fio_tls_pkey = EVP_PKEY_new();
  FIO_ASSERT(fio_tls_pkey, "OpenSSL failed to create private key.");

  /* TODO: replace RSA with something else? is there something else? */
  RSA *rsa = RSA_new();
  BIGNUM *e = BN_new();
  BN_clear(e);
  BN_add_word(e, 65537);
  FIO_ASSERT_ALLOC(e);
  FIO_ASSERT(RSA_generate_key_ex(rsa, 8192, e, NULL),
             "OpenSSL failed to create RSA key.");
  BN_free(e);
  EVP_PKEY_assign_RSA(fio_tls_pkey, rsa);
  fio_state_callback_add(FIO_CALL_AT_EXIT, fio_tls_clear_root_key,
                         fio_tls_pkey);
finish:
  fio_unlock(&lock);
}

static X509 *fio_tls_create_self_signed(char *server_name) {
  X509 *cert = X509_new();
  static uint32_t counter = 0;
  FIO_ASSERT(cert,
             "OpenSSL failed to allocate memory for self-signed ceritifcate.");
  fio_tls_make_root_key();

  /* serial number */
  fio_atomic_add(&counter, 1);
  ASN1_INTEGER_set(X509_get_serialNumber(cert), counter);

  /* validity (180 days) */
  X509_gmtime_adj(X509_get_notBefore(cert), 0);
  X509_gmtime_adj(X509_get_notAfter(cert), 15552000L);

  /* set (public) key */
  X509_set_pubkey(cert, fio_tls_pkey);

  /* set identity details */
  X509_NAME *s = X509_get_subject_name(cert);
  size_t srv_name_len = strlen(server_name);
  X509_NAME_add_entry_by_txt(s, "O", MBSTRING_ASC, (unsigned char *)server_name,
                             srv_name_len, -1, 0);
  X509_NAME_add_entry_by_txt(s, "CN", MBSTRING_ASC,
                             (unsigned char *)server_name, srv_name_len, -1, 0);
  X509_NAME_add_entry_by_txt(s, "CA", MBSTRING_ASC,
                             (unsigned char *)server_name, srv_name_len, -1, 0);
  X509_set_issuer_name(cert, s);

  /* sign certificate */
  FIO_ASSERT(X509_sign(cert, fio_tls_pkey, EVP_sha512()),
             "OpenSSL failed to signe self-signed certificate");
  // FILE *fp = fopen("tmp.pem", "ab+");
  // if (fp) {
  //   PEM_write_X509(fp, cert);
  //   fclose(fp);
  // }

  return cert;
}

/* *****************************************************************************
SSL/TLS Context (re)-building
***************************************************************************** */

#define TLS_BUFFER_LENGTH (1 << 15)
typedef struct {
  SSL *ssl;
  fio_tls_s *tls;
  void *alpn_arg;
  intptr_t uuid;
  uint8_t is_server;
  volatile uint8_t alpn_ok;
} fio_tls_connection_s;

static void fio_tls_alpn_fallback(fio_tls_connection_s *c) {
  fio_tls_s *tls = c->tls;
  if (alpn_ary_count(&tls->alpn) == 0)
    return;
  /* set protocol to default protocol */
  if (alpn_ary_get(&tls->alpn, 0).callback) {
    FIO_LOG_DEBUG("TLS ALPN handshake missing, falling back on %s for %p",
                  fio_str_info(&alpn_ary_to_a(&tls->alpn)->name).data,
                  (void *)c->uuid);
    alpn_ary_get(&tls->alpn, 0).callback(c->uuid, c->alpn_arg);
  }
  c->alpn_ok = 1;
}
static int fio_tls_alpn_selector_cb(SSL *ssl, const unsigned char **out,
                                    unsigned char *outlen,
                                    const unsigned char *in, unsigned int inlen,
                                    void *tls_) {
  fio_tls_s *tls = tls_;
  /* TODO: select ALPN and call callback */
  fio_tls_connection_s *c = SSL_get_ex_data(ssl, 0);

  if (alpn_ary_count(&tls->alpn) == 0)
    return SSL_TLSEXT_ERR_NOACK;
  const unsigned char *end = in + inlen;
  while (in < end) {
    uint8_t l = in[0];
    fio_str_s tmp = FIO_STR_INIT_STATIC2((char *)in + 1, l);
    for (size_t i = 0; i < alpn_ary_count(&tls->alpn); ++i) {
      alpn_s *a = alpn_ary_to_a(&tls->alpn) + i;
      if (fio_str_iseq(&a->name, &tmp)) {
        /* we have a match */
        fio_str_info_s info = fio_str_info(&a->name);
        *out = (unsigned char *)info.data;
        *outlen = (unsigned char)info.len;
        FIO_LOG_DEBUG("TLS ALPN set to: %s for %p", info.data, (void *)c->uuid);
        c->alpn_ok = 1;
        if (a->callback)
          a->callback(c->uuid, c->alpn_arg);
        return SSL_TLSEXT_ERR_OK;
      }
    }
    in += l + 1;
  }
  /* set protocol to default protocol */
  if (alpn_ary_get(&tls->alpn, 0).callback) {
    FIO_LOG_DEBUG("TLS ALPN handshake failed, falling back on default for %p",
                  (void *)c->uuid);
    alpn_ary_get(&tls->alpn, 0).callback(c->uuid, c->alpn_arg);
  }
  return SSL_TLSEXT_ERR_NOACK;
  (void)ssl;
  (void)out;
  (void)outlen;
  (void)in;
  (void)inlen;
  (void)tls;
}

/** Called when the library specific data for the context should be destroyed */
static void fio_tls_destroy_context(fio_tls_s *tls) {
  /* TODO: Library specific implementation */
  SSL_CTX_free(tls->ctx);
  free(tls->alpn_str);

  tls->ctx = NULL;
  tls->alpn_str = NULL;
  tls->alpn_len = 0;
  FIO_LOG_DEBUG("destroyed TLS context for OpenSSL %p", (void *)tls);
}

static int fio_tls_pem_passwd_cb(char *buf, int size, int rwflag,
                                 void *password) {
  fio_str_info_s *p = password;
  if (!p || !p->len || !size)
    return 0;
  int len = (size <= (int)p->len) ? (size - 1) : (int)p->len;
  memcpy(buf, p->data, len);
  buf[len] = 0;
  return len;
  (void)rwflag;
}

/** Called when the library specific data for the context should be built */
static void fio_tls_build_context(fio_tls_s *tls) {
  fio_tls_destroy_context(tls);
  /* TODO: Library specific implementation */

  /* create new context */
  tls->ctx = SSL_CTX_new(TLS_method());
  SSL_CTX_set_mode(tls->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
  /* see: https://caniuse.com/#search=tls */
  SSL_CTX_set_min_proto_version(tls->ctx, TLS1_2_VERSION);
  SSL_CTX_set_options(tls->ctx, SSL_OP_NO_COMPRESSION);

  /* attach certificates */
  FIO_ARY_FOR(&tls->sni, pos) {
    fio_str_info_s keys[4] = {
        fio_str_info(&pos->private_key), fio_str_info(&pos->public_key),
        fio_str_info(&pos->password),
        /* empty password slot for public key */
    };
    if (keys[0].len && keys[1].len) {
      if (1) {
        /* Extract private key from private key file */
        BIO *bio = BIO_new_mem_buf(keys[0].data, keys[0].len);
        if (bio) {
          EVP_PKEY *k = PEM_read_bio_PrivateKey(
              bio, NULL, fio_tls_pem_passwd_cb, keys + 2);
          if (k) {
            FIO_LOG_DEBUG("TLS read private key from PEM file.");
            SSL_CTX_use_PrivateKey(tls->ctx, k);
          }
          BIO_free(bio);
        }
      }
      /* Certificate Files loaded */
      for (int ki = 0; ki < 2; ++ki) {
        /* Extract as much data as possible from each file */
        BIO *bio = BIO_new_mem_buf(keys[ki].data, keys[ki].len);
        FIO_ASSERT(bio, "OpenSSL error allocating BIO.");
        STACK_OF(X509_INFO) *inf = PEM_X509_INFO_read_bio(
            bio, NULL, fio_tls_pem_passwd_cb, keys + ki + 2);
        if (inf) {
          for (int i = 0; i < sk_X509_INFO_num(inf); ++i) {
            /* for each element in PEM */
            X509_INFO *tmp = sk_X509_INFO_value(inf, i);
            if (tmp->x509) {
              FIO_LOG_DEBUG("TLS adding certificate from PEM file.");
              SSL_CTX_use_certificate(tls->ctx, tmp->x509);
            }
            if (tmp->x_pkey) {
              FIO_LOG_DEBUG("TLS adding private key from PEM file.");
              SSL_CTX_use_PrivateKey(tls->ctx, tmp->x_pkey->dec_pkey);
            }
          }
          sk_X509_INFO_pop_free(inf, X509_INFO_free);
        }
        BIO_free(bio);
      }
    } else if (keys[0].len) {
      /* Self Signed Certificates, only if server name is provided. */
      SSL_CTX_use_certificate(tls->ctx,
                              fio_tls_create_self_signed(keys[0].data));
      SSL_CTX_use_PrivateKey(tls->ctx, fio_tls_pkey);
    }
  }

  /* setup ALPN support */
  if (1) {
    size_t alpn_pos = 0;
    intptr_t i = alpn_ary_count(&tls->alpn);
    while (i) {
      --i;
      fio_str_info_s s = fio_str_info(&alpn_ary_to_a(&tls->alpn)[i].name);
      if (!s.len)
        continue;
      tls->alpn_str = realloc(tls->alpn_str, alpn_pos + 1 + s.len);
      tls->alpn_str[alpn_pos++] = s.len;
      memcpy(tls->alpn_str + alpn_pos, s.data, s.len);
      alpn_pos += 1 + s.len;
    }
    tls->alpn_len = alpn_pos;
    SSL_CTX_set_alpn_select_cb(tls->ctx, fio_tls_alpn_selector_cb, tls);
    SSL_CTX_set_alpn_protos(tls->ctx, tls->alpn_str, tls->alpn_len);
  }

  /* Peer Verification / Trust */
  if (trust_ary_count(&tls->trust)) {
    /* TODO: enable peer verification */
    X509_STORE *store = X509_STORE_new();
    SSL_CTX_set_cert_store(tls->ctx, store);
    SSL_CTX_set_verify(tls->ctx, SSL_VERIFY_PEER, NULL);
    /* TODO: Add each ceriticate in the PEM to the trust "store" */
    FIO_ARY_FOR(&tls->trust, pos) {
      fio_str_info_s pem = fio_str_info(&pos->pem);
      BIO *bio = BIO_new_mem_buf(pem.data, pem.len);
      FIO_ASSERT(bio, "OpenSSL error allocating BIO.");
      STACK_OF(X509_INFO) *inf = PEM_X509_INFO_read_bio(bio, NULL, NULL, NULL);
      if (inf) {
        for (int i = 0; i < sk_X509_INFO_num(inf); ++i) {
          /* for each element in PEM */
          X509_INFO *tmp = sk_X509_INFO_value(inf, i);
          if (tmp->x509) {
            FIO_LOG_DEBUG("TLS trusting certificate from PEM file.");
            X509_STORE_add_cert(store, tmp->x509);
          }
          if (tmp->crl) {
            X509_STORE_add_crl(store, tmp->crl);
          }
        }
        sk_X509_INFO_pop_free(inf, X509_INFO_free);
      }
      BIO_free(bio);
    }
  }

  FIO_LOG_DEBUG("(re)built TLS context for OpenSSL %p", (void *)tls);
}

/* *****************************************************************************
SSL/TLS RW Hooks
***************************************************************************** */

static void fio_tls_delayed_close(void *uuid, void *ignr_) {
  fio_close((intptr_t)uuid);
  (void)ignr_;
}

/* TODO: this is an example implementation - fix for specific library. */

/**
 * Implement reading from a file descriptor. Should behave like the file
 * system `read` call, including the setup or errno to EAGAIN / EWOULDBLOCK.
 *
 * Note: facil.io library functions MUST NEVER be called by any r/w hook, or a
 * deadlock might occur.
 */
static ssize_t fio_tls_read(intptr_t uuid, void *udata, void *buf,
                            size_t count) {
  fio_tls_connection_s *c = udata;
  ssize_t ret = SSL_read(c->ssl, buf, count);
  if (ret > 0)
    return ret;
  ret = SSL_get_error(c->ssl, ret);
  switch (ret) {
  case SSL_ERROR_SSL: /* overflow */
  case SSL_ERROR_ZERO_RETURN:
    return 0;                      /* EOF */
  case SSL_ERROR_NONE:             /* overflow */
  case SSL_ERROR_WANT_CONNECT:     /* overflow */
  case SSL_ERROR_WANT_ACCEPT:      /* overflow */
  case SSL_ERROR_WANT_X509_LOOKUP: /* overflow */
  case SSL_ERROR_WANT_ASYNC:       /* overflow */
  case SSL_ERROR_WANT_WRITE:       /* overflow */
  case SSL_ERROR_WANT_READ:
  default:
    break;
  }
  errno = EWOULDBLOCK;
  return -1;
  (void)uuid;
}

/**
 * When implemented, this function will be called to flush any data remaining
 * in the internal buffer.
 *
 * The function should return the number of bytes remaining in the internal
 * buffer (0 is a valid response) or -1 (on error).
 *
 * Note: facil.io library functions MUST NEVER be called by any r/w hook, or a
 * deadlock might occur.
 */
static ssize_t fio_tls_flush(intptr_t uuid, void *udata) {
  (void)uuid;
  (void)udata;
  return 0;
}

/**
 * Implement writing to a file descriptor. Should behave like the file system
 * `write` call.
 *
 * If an internal buffer is implemented and it is full, errno should be set to
 * EWOULDBLOCK and the function should return -1.
 *
 * Note: facil.io library functions MUST NEVER be called by any r/w hook, or a
 * deadlock might occur.
 */
static ssize_t fio_tls_write(intptr_t uuid, void *udata, const void *buf,
                             size_t count) {
  fio_tls_connection_s *c = udata;
  ssize_t ret = SSL_write(c->ssl, buf, count);
  if (ret > 0)
    return ret;
  ret = SSL_get_error(c->ssl, ret);
  switch (ret) {
  case SSL_ERROR_SSL: /* overflow */
  case SSL_ERROR_ZERO_RETURN:
    return 0;                      /* EOF */
  case SSL_ERROR_NONE:             /* overflow */
  case SSL_ERROR_WANT_CONNECT:     /* overflow */
  case SSL_ERROR_WANT_ACCEPT:      /* overflow */
  case SSL_ERROR_WANT_X509_LOOKUP: /* overflow */
  case SSL_ERROR_WANT_ASYNC:       /* overflow */
  case SSL_ERROR_WANT_WRITE:       /* overflow */
  case SSL_ERROR_WANT_READ:
  default:
    break;
  }
  errno = EWOULDBLOCK;
  return -1;
  (void)uuid;
}

/**
 * The `close` callback should close the underlying socket / file descriptor.
 *
 * If the function returns a non-zero value, it will be called again after an
 * attempt to flush the socket and any pending outgoing buffer.
 *
 * Note: facil.io library functions MUST NEVER be called by any r/w hook, or a
 * deadlock might occur.
 * */
static ssize_t fio_tls_before_close(intptr_t uuid, void *udata) {
  fio_tls_connection_s *c = udata;
  SSL_shutdown(c->ssl);
  return 1;
  (void)uuid;
}
/**
 * Called to perform cleanup after the socket was closed.
 * */
static void fio_tls_cleanup(void *udata) {
  fio_tls_connection_s *c = udata;
  SSL_free(c->ssl);
  FIO_LOG_DEBUG("TLS cleanup for %p", (void *)c->uuid);
  free(udata);
}

static fio_rw_hook_s FIO_TLS_HOOKS = {
    .read = fio_tls_read,
    .write = fio_tls_write,
    .before_close = fio_tls_before_close,
    .flush = fio_tls_flush,
    .cleanup = fio_tls_cleanup,
};

static size_t fio_tls_handshake(intptr_t uuid, void *udata, uint8_t schedule) {
  fio_tls_connection_s *c = udata;
  int ri;
  if (c->is_server) {
    ri = SSL_accept(c->ssl);
  } else {
    ri = SSL_connect(c->ssl);
  }
  if (ri != 1) {
    ri = SSL_get_error(c->ssl, ri);
    switch (ri) {
    case SSL_ERROR_SSL:
      FIO_LOG_DEBUG("SSL_accept/SSL_connect %p error: SSL_ERROR_SSL",
                    (void *)uuid);
      fio_defer(fio_tls_delayed_close, (void *)uuid, NULL);
      break;
    case SSL_ERROR_ZERO_RETURN:
      FIO_LOG_DEBUG("SSL_accept/SSL_connect %p error: SSL_ERROR_ZERO_RETURN",
                    (void *)uuid);
      fio_defer(fio_tls_delayed_close, (void *)uuid, NULL);
      break;
    case SSL_ERROR_NONE:
      FIO_LOG_DEBUG("SSL_accept/SSL_connect %p error: SSL_ERROR_NONE",
                    (void *)uuid);
      break;
    case SSL_ERROR_WANT_CONNECT:
      FIO_LOG_DEBUG("SSL_accept/SSL_connect %p error: SSL_ERROR_WANT_CONNECT",
                    (void *)uuid);
      break;
    case SSL_ERROR_WANT_ACCEPT:
      FIO_LOG_DEBUG("SSL_accept/SSL_connect %p error: SSL_ERROR_WANT_ACCEPT",
                    (void *)uuid);
      break;
    case SSL_ERROR_WANT_X509_LOOKUP:
      FIO_LOG_DEBUG(
          "SSL_accept/SSL_connect %p error: SSL_ERROR_WANT_X509_LOOKUP",
          (void *)uuid);
      break;
    case SSL_ERROR_WANT_ASYNC:
      FIO_LOG_DEBUG("SSL_accept/SSL_connect %p error: SSL_ERROR_WANT_ASYNC",
                    (void *)uuid);
      break;
    case SSL_ERROR_WANT_WRITE:
      // FIO_LOG_DEBUG("SSL_accept/SSL_connect %p state: SSL_ERROR_WANT_WRITE",
      //               (void *)uuid);
      // if (schedule)
      //   fio_force_event(uuid, FIO_EVENT_ON_READY);
      break;
    case SSL_ERROR_WANT_READ:
      // FIO_LOG_DEBUG("SSL_accept/SSL_connect %p state: SSL_ERROR_WANT_READ",
      //               (void *)uuid);
      if (schedule)
        fio_force_event(uuid, FIO_EVENT_ON_DATA);
      break;
    default:
      FIO_LOG_DEBUG("SSL_accept/SSL_connect %p error: unknown.", (void *)uuid);
      fio_defer(fio_tls_delayed_close, (void *)uuid, NULL);
      break;
    }
    return 0;
  }
  if (!c->alpn_ok)
    fio_tls_alpn_fallback(c);
  if (fio_rw_hook_replace_unsafe(uuid, &FIO_TLS_HOOKS, udata) == 0) {
    FIO_LOG_DEBUG("Completed TLS handshake for %p", (void *)uuid);
  } else {
    FIO_LOG_DEBUG("Something went wrong during TLS handshake for %p",
                  (void *)uuid);
    return 0;
  }
  return 1;
}

static ssize_t fio_tls_read4handshake(intptr_t uuid, void *udata, void *buf,
                                      size_t count) {
  // FIO_LOG_DEBUG("TLS handshake from read %p", (void *)uuid);
  if (fio_tls_handshake(uuid, udata, 0))
    return fio_tls_read(uuid, udata, buf, count);
  errno = EWOULDBLOCK;
  return -1;
}

static ssize_t fio_tls_write4handshake(intptr_t uuid, void *udata,
                                       const void *buf, size_t count) {
  // FIO_LOG_DEBUG("TLS handshake from write %p", (void *)uuid);
  if (fio_tls_handshake(uuid, udata, 0))
    return fio_tls_write(uuid, udata, buf, count);
  errno = EWOULDBLOCK;
  return -1;
}

static ssize_t fio_tls_flush4handshake(intptr_t uuid, void *udata) {
  // FIO_LOG_DEBUG("TLS handshake from flush %p", (void *)uuid);
  if (fio_tls_handshake(uuid, udata, 1))
    return fio_tls_flush(uuid, udata);
  errno = 0;
  return 0;
}
static fio_rw_hook_s FIO_TLS_HANDSHAKE_HOOKS = {
    .read = fio_tls_read4handshake,
    .write = fio_tls_write4handshake,
    .before_close = fio_tls_before_close,
    .flush = fio_tls_flush4handshake,
    .cleanup = fio_tls_cleanup,
};
static inline void fio_tls_attach2uuid(intptr_t uuid, fio_tls_s *tls,
                                       void *udata, uint8_t is_server) {
  /* create SSL connection context from global context */
  fio_tls_connection_s *c = malloc(sizeof(*c));
  FIO_ASSERT_ALLOC(c);
  *c = (fio_tls_connection_s){
      .alpn_arg = udata,
      .tls = tls,
      .uuid = uuid,
      .ssl = SSL_new(tls->ctx),
      .is_server = is_server,
      .alpn_ok = 0,
  };
  FIO_ASSERT_ALLOC(c->ssl);
  /* set facil.io data in the SSL object */
  SSL_set_ex_data(c->ssl, 0, (void *)c);
  /* attach socket - TODO: Switch to BIO socket */
  BIO *bio = BIO_new_socket(fio_uuid2fd(uuid), 0);
  BIO_up_ref(bio);
  SSL_set0_rbio(c->ssl, bio);
  SSL_set0_wbio(c->ssl, bio);
  /* set RW hooks */
  fio_rw_hook_set(uuid, &FIO_TLS_HANDSHAKE_HOOKS, c);
  if (is_server) {
    /* Server mode (accept) */
    FIO_LOG_DEBUG("Attaching TLS read/write hook for %p (server mode).",
                  (void *)uuid);
    SSL_set_accept_state(c->ssl);
  } else {
    /* Client mode (connect) */
    FIO_LOG_DEBUG("Attaching TLS read/write hook for %p (client mode).",
                  (void *)uuid);
    SSL_set_connect_state(c->ssl);
  }
  fio_force_event(uuid, FIO_EVENT_ON_READY);
}

/* *****************************************************************************
SSL/TLS API implementation - this can be pretty much used as is...
***************************************************************************** */

/**
 * Creates a new SSL/TLS context / settings object with a default certificate
 * (if any).
 */
fio_tls_s *FIO_TLS_WEAK fio_tls_new(const char *server_name, const char *key,
                                    const char *cert, const char *pk_password) {
  REQUIRE_LIBRARY();
  fio_tls_s *tls = calloc(sizeof(*tls), 1);
  fio_tls_cert_add(tls, server_name, key, cert, pk_password);
  return tls;
}

/**
 * Adds a certificate  a new SSL/TLS context / settings object.
 *
 *      fio_tls_cert_add(tls, FIO_TLS_CERT("www.example.com",
 *                            "private_key.key",
 *                            "public_key.crt" ));
 */
void FIO_TLS_WEAK fio_tls_cert_add(fio_tls_s *tls, const char *server_name,
                                   const char *key, const char *cert,
                                   const char *pk_password) {
  REQUIRE_LIBRARY();
  cert_s c = {
      .private_key = FIO_STR_INIT,
      .public_key = FIO_STR_INIT,
      .password = FIO_STR_INIT_STATIC2(pk_password,
                                       (pk_password ? strlen(pk_password) : 0)),
  };
  if (key && cert) {
    if (fio_str_readfile(&c.private_key, key, 0, 0).data == NULL)
      goto file_missing;
    if (fio_str_readfile(&c.public_key, cert, 0, 0).data == NULL)
      goto file_missing;
    cert_ary_push(&tls->sni, c);
  } else if (server_name) {
    /* Self-Signed TLS Certificates */
    c.private_key = FIO_STR_INIT_STATIC(server_name);
    cert_ary_push(&tls->sni, c);
  }
  fio_tls_cert_destroy(&c);
  fio_tls_build_context(tls);
  return;
file_missing:
  FIO_LOG_FATAL("TLS certificate file missing for either %s or %s or both.",
                key, cert);
  exit(-1);
}

/**
 * Adds an ALPN protocol callback to the SSL/TLS context.
 *
 * The first protocol added will act as the default protocol to be selected.
 */
void FIO_TLS_WEAK fio_tls_proto_add(fio_tls_s *tls, const char *protocol_name,
                                    void (*callback)(intptr_t uuid,
                                                     void *udata)) {
  REQUIRE_LIBRARY();
  alpn_s tmp = {
      .name = FIO_STR_INIT_STATIC(protocol_name),
      .callback = callback,
  };
  if (fio_str_len(&tmp.name) > 255) {
    FIO_LOG_ERROR(
        "fio_tls_proto_add called with a protocol name exceeding 255 bytes.");
    return;
  }
  alpn_ary_push(&tls->alpn, tmp);
  fio_alpn_destroy(&tmp);
  fio_tls_build_context(tls);
}

/**
 * Adds a certificate to the "trust" list, which automatically adds a peer
 * verification requirement.
 *
 *      fio_tls_trust(tls, "google-ca.pem" );
 */
void FIO_TLS_WEAK fio_tls_trust(fio_tls_s *tls, const char *public_cert_file) {
  REQUIRE_LIBRARY();
  trust_s c = {
      .pem = FIO_STR_INIT,
  };
  if (!public_cert_file)
    return;
  if (fio_str_readfile(&c.pem, public_cert_file, 0, 0).data == NULL)
    goto file_missing;
  trust_ary_push(&tls->trust, c);
  fio_tls_trust_destroy(&c);
  fio_tls_build_context(tls);
  return;
file_missing:
  FIO_LOG_FATAL("TLS certificate file missing for %s ", public_cert_file);
  exit(-1);
}

/**
 * Establishes an SSL/TLS connection as an SSL/TLS Server, using the specified
 * context / settings object.
 *
 * The `uuid` should be a socket UUID that is already connected to a peer (i.e.,
 * the result of `fio_accept`).
 *
 * The `udata` is an opaque user data pointer that is passed along to the
 * protocol selected (if any protocols were added using `fio_tls_proto_add`).
 */
void FIO_TLS_WEAK fio_tls_accept(intptr_t uuid, fio_tls_s *tls, void *udata) {
  REQUIRE_LIBRARY();
  fio_tls_attach2uuid(uuid, tls, udata, 1);
}

/**
 * Establishes an SSL/TLS connection as an SSL/TLS Client, using the specified
 * context / settings object.
 *
 * The `uuid` should be a socket UUID that is already connected to a peer (i.e.,
 * one received by a `fio_connect` specified callback `on_connect`).
 *
 * The `udata` is an opaque user data pointer that is passed along to the
 * protocol selected (if any protocols were added using `fio_tls_proto_add`).
 */
void FIO_TLS_WEAK fio_tls_connect(intptr_t uuid, fio_tls_s *tls, void *udata) {
  REQUIRE_LIBRARY();
  fio_tls_attach2uuid(uuid, tls, udata, 0);
}

/**
 * Destroys the SSL/TLS context / settings object and frees any related
 * resources / memory.
 */
void FIO_TLS_WEAK fio_tls_destroy(fio_tls_s *tls) {
  if (!tls)
    return;
  REQUIRE_LIBRARY();
  fio_tls_destroy_context(tls);
  alpn_ary_free(&tls->alpn);
  cert_ary_free(&tls->sni);
  free(tls);
}

#endif /* Library compiler flags */
