/*
 * cscreen -- a tiny, near-dependency-free screen sharing relay.
 *
 * Build (plain, for localhost / no HTTPS):
 *     gcc -o cscreen -lpthread cscreen.c
 *
 * Build (with TLS, for a public relay -- adds OpenSSL):
 *     gcc -DUSE_TLS -o cscreen -lpthread -lssl -lcrypto cscreen.c
 *
 * Run:
 *     cscreen 5050 5051                                  (no gate, no TLS)
 *     cscreen 5050 5051 secretword                       (access code)
 *     cscreen 5050 5051 fullchain.pem privkey.pem        (HTTPS)
 *     cscreen 5050 5051 secretword fullchain.pem privkey.pem
 *
 *     5050 = HTTP(S) port (serves the browser client)
 *     5051 = relay port (WebSocket, carries opaque media blocks)
 *
 * The relay knows nothing about video.  It shuttles opaque binary blocks
 * from the one designated sharer to every viewer, and understands a
 * handful of tiny text control messages.
 *
 * Pure C89 + pthreads + BSD sockets.  The one optional extra is OpenSSL,
 * compiled in only with -DUSE_TLS: browsers refuse getDisplayMedia() over
 * plain http to any host but localhost, so a relay on a real machine has
 * to speak https.  A clean-room TLS stack is out of scope, hence -lssl.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#ifdef USE_TLS
#include <fcntl.h>
#include <poll.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif


#define MAX_CONNS        64
#define OUT_LIMIT        (24 * 1024 * 1024)   /* per-viewer send backlog cap */
#define MAX_MESSAGE      (64 * 1024 * 1024)   /* largest inbound ws message  */
#define PING_SECONDS     5
#define DEAD_SECONDS     30
#define MIME_MAX         128
#define CODE_LEN         10                    /* hex digits in access code   */


/* ------------------------------------------------------------------ */
/* SHA-1 (needed only for the WebSocket handshake)                     */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long h[5];
    unsigned char block[64];
    unsigned int blockLen;
    unsigned long totalBits;
} Sha1;

#define ROL32( v, n )  ( ( ( (v) << (n) ) | ( (v) >> ( 32 - (n) ) ) ) & 0xFFFFFFFFUL )

static void sha1_init( Sha1 *c ) {
    c->h[0] = 0x67452301UL;
    c->h[1] = 0xEFCDAB89UL;
    c->h[2] = 0x98BADCFEUL;
    c->h[3] = 0x10325476UL;
    c->h[4] = 0xC3D2E1F0UL;
    c->blockLen = 0;
    c->totalBits = 0;
    }

static void sha1_block( Sha1 *c, const unsigned char *p ) {
    unsigned long w[80];
    unsigned long a, b, d, e, f, k, t;
    int i;

    for( i = 0; i < 16; i++ ) {
        w[i] = ( (unsigned long)p[ i * 4 ] << 24 ) |
               ( (unsigned long)p[ i * 4 + 1 ] << 16 ) |
               ( (unsigned long)p[ i * 4 + 2 ] << 8 ) |
               ( (unsigned long)p[ i * 4 + 3 ] );
        }
    for( i = 16; i < 80; i++ ) {
        w[i] = ROL32( w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1 );
        }

    a = c->h[0]; b = c->h[1]; d = c->h[2]; e = c->h[3]; f = c->h[4];

    for( i = 0; i < 80; i++ ) {
        unsigned long fn;
        if( i < 20 ) { fn = ( b & d ) | ( ( ~b ) & e ); k = 0x5A827999UL; }
        else if( i < 40 ) { fn = b ^ d ^ e; k = 0x6ED9EBA1UL; }
        else if( i < 60 ) { fn = ( b & d ) | ( b & e ) | ( d & e ); k = 0x8F1BBCDCUL; }
        else { fn = b ^ d ^ e; k = 0xCA62C1D6UL; }

        t = ( ROL32( a, 5 ) + ( fn & 0xFFFFFFFFUL ) + f + k + w[i] ) & 0xFFFFFFFFUL;
        f = e;
        e = d;
        d = ROL32( b, 30 );
        b = a;
        a = t;
        }

    c->h[0] = ( c->h[0] + a ) & 0xFFFFFFFFUL;
    c->h[1] = ( c->h[1] + b ) & 0xFFFFFFFFUL;
    c->h[2] = ( c->h[2] + d ) & 0xFFFFFFFFUL;
    c->h[3] = ( c->h[3] + e ) & 0xFFFFFFFFUL;
    c->h[4] = ( c->h[4] + f ) & 0xFFFFFFFFUL;
    }

static void sha1_update( Sha1 *c, const unsigned char *p, unsigned int len ) {
    unsigned int i;
    for( i = 0; i < len; i++ ) {
        c->block[ c->blockLen++ ] = p[i];
        c->totalBits += 8;
        if( c->blockLen == 64 ) {
            sha1_block( c, c->block );
            c->blockLen = 0;
            }
        }
    }

static void sha1_final( Sha1 *c, unsigned char out[20] ) {
    unsigned long bits = c->totalBits;
    unsigned char pad = 0x80;
    unsigned char zero = 0x00;
    unsigned char lenBytes[8];
    int i;

    sha1_update( c, &pad, 1 );
    while( c->blockLen != 56 ) {
        sha1_update( c, &zero, 1 );
        }
    for( i = 0; i < 8; i++ ) {
        lenBytes[i] = 0;
        }
    lenBytes[4] = (unsigned char)( ( bits >> 24 ) & 0xFF );
    lenBytes[5] = (unsigned char)( ( bits >> 16 ) & 0xFF );
    lenBytes[6] = (unsigned char)( ( bits >> 8 ) & 0xFF );
    lenBytes[7] = (unsigned char)( bits & 0xFF );

    /* feed the length without letting totalBits interfere */
    for( i = 0; i < 8; i++ ) {
        c->block[ c->blockLen++ ] = lenBytes[i];
        }
    sha1_block( c, c->block );
    c->blockLen = 0;

    for( i = 0; i < 5; i++ ) {
        out[ i * 4 ] = (unsigned char)( ( c->h[i] >> 24 ) & 0xFF );
        out[ i * 4 + 1 ] = (unsigned char)( ( c->h[i] >> 16 ) & 0xFF );
        out[ i * 4 + 2 ] = (unsigned char)( ( c->h[i] >> 8 ) & 0xFF );
        out[ i * 4 + 3 ] = (unsigned char)( c->h[i] & 0xFF );
        }
    }


static const char *B64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode( const unsigned char *in, int len, char *out ) {
    int i = 0;
    int o = 0;
    while( i < len ) {
        unsigned long v = 0;
        int n = 0;
        int j;
        for( j = 0; j < 3; j++ ) {
            v <<= 8;
            if( i < len ) {
                v |= in[ i++ ];
                n++;
                }
            }
        out[ o++ ] = B64[ ( v >> 18 ) & 0x3F ];
        out[ o++ ] = B64[ ( v >> 12 ) & 0x3F ];
        out[ o++ ] = ( n > 1 ) ? B64[ ( v >> 6 ) & 0x3F ] : '=';
        out[ o++ ] = ( n > 2 ) ? B64[ v & 0x3F ] : '=';
        }
    out[o] = '\0';
    }


/* ------------------------------------------------------------------ */
/* Connection bookkeeping                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int used;
    int id;
    int sock;
    int isSharer;
    int inSync;          /* has been told to reset, so may receive media */
    int closing;

    void *ssl;           /* SSL* when TLS is active, else NULL          */
    pthread_mutex_t sslLock;  /* serialises the one SSL object's read/  */
                              /* write, held only across a single SSL   */
                              /* call so neither thread ever blocks the */
                              /* other on the poll wait                 */

    unsigned char *out;  /* pending bytes to write                      */
    size_t outLen;
    size_t outCap;

    time_t lastSeen;

    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t writer;
    } Conn;


static Conn gConns[ MAX_CONNS ];
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static int gNextId = 1;
static int gSharerId = 0;          /* 0 == nobody sharing               */
static int gAwaitStart = 0;        /* restart requested, not yet begun  */
static time_t gAwaitSince = 0;     /* when that request went out        */
static char gMime[ MIME_MAX ];
static int gHttpPort = 0;
static int gRelayPort = 0;

/* Non-zero once a certificate has been loaded, so both listeners wrap    */
/* every accepted socket in TLS.  Only ever set in a -DUSE_TLS build.     */
static int gTLS = 0;
#ifdef USE_TLS
static SSL_CTX *gCtx = NULL;
#endif

/* Access code derived from the optional security string on the command   */
/* line.  Empty means "no gate, serve anybody".  When set, both the HTTP  */
/* page and the relay WebSocket demand it as the URL path.                */
static char gCode[ CODE_LEN + 1 ] = "";


/* ------------------------------------------------------------------ */
/* Access code                                                         */
/* ------------------------------------------------------------------ */

/* Fold the user's secret down to CODE_LEN uppercase hex digits.  This is */
/* deterministic across runs on purpose: a cron job that restarts the     */
/* relay with the same secret keeps handing out the same secret URL, so   */
/* the humans never have to be told a new one.                            */
static void derive_code( const char *secret, char *out ) {
    static const char *hex = "0123456789ABCDEF";
    unsigned char digest[20];
    Sha1 ctx;
    int i;

    sha1_init( &ctx );
    sha1_update( &ctx, (const unsigned char *)secret,
                 (unsigned int)strlen( secret ) );
    sha1_final( &ctx, digest );

    for( i = 0; i < CODE_LEN; i++ ) {
        /* One hex digit per nibble, walking the front of the digest. */
        unsigned char b = digest[ i / 2 ];
        out[i] = hex[ ( i % 2 ) == 0 ? ( b >> 4 ) : ( b & 0x0F ) ];
        }
    out[ CODE_LEN ] = '\0';
    }


/* Pull the path out of a request line ("GET /foo?x HTTP/1.1") into buf, */
/* stripping the query string and any trailing slashes.  Returns 0 if    */
/* the request does not look like a request line at all.                 */
static int request_path( const char *req, char *buf, size_t bufSize ) {
    const char *p = strchr( req, ' ' );
    const char *end;
    size_t len;

    if( p == NULL ) {
        return 0;
        }
    p++;
    end = p;
    while( *end != '\0' && *end != ' ' && *end != '?' &&
           *end != '\r' && *end != '\n' ) {
        end++;
        }
    len = (size_t)( end - p );
    if( len >= bufSize ) {
        return 0;
        }
    memcpy( buf, p, len );
    buf[len] = '\0';

    while( len > 1 && buf[ len - 1 ] == '/' ) {
        buf[ --len ] = '\0';
        }
    return 1;
    }


/* Does this request carry the access code as its path?  Always true when */
/* no security string was given.  Comparison is case insensitive, because */
/* a human is going to be retyping this over the phone.                   */
static int path_authorized( const char *req ) {
    char path[256];
    int i;

    if( gCode[0] == '\0' ) {
        return 1;
        }
    if( !request_path( req, path, sizeof( path ) ) ) {
        return 0;
        }
    if( path[0] != '/' ) {
        return 0;
        }
    for( i = 0; i < CODE_LEN; i++ ) {
        char a = path[ i + 1 ];
        if( a >= 'a' && a <= 'z' ) {
            a = (char)( a - 'a' + 'A' );
            }
        if( a != gCode[i] ) {
            return 0;
            }
        }
    return path[ CODE_LEN + 1 ] == '\0';
    }


/* ------------------------------------------------------------------ */
/* Low level I/O -- transparently plain TCP or TLS                     */
/*                                                                     */
/* Every read and write goes through these three wrappers.  When the   */
/* ssl argument is NULL the fd is an ordinary blocking socket and we    */
/* use send()/recv() exactly as before.  When ssl is non-NULL (only    */
/* reachable in a -DUSE_TLS build) the fd is non-blocking and each SSL  */
/* call is retried around poll().  The lock, when given, is taken only  */
/* for the duration of one SSL call and never across the poll: that is  */
/* what lets the relay's reader thread sit parked waiting for bytes     */
/* without ever stopping the writer thread from sending, even though    */
/* the two share a single SSL object.                                  */
/* ------------------------------------------------------------------ */

static void set_nodelay( int sock ) {
    int one = 1;
    setsockopt( sock, IPPROTO_TCP, TCP_NODELAY, (void *)&one, sizeof( one ) );
    }


#ifdef USE_TLS
/* Wait up to a second for the socket to become readable or writable.
   Returns poll()'s result: >0 ready, 0 timeout, <0 interrupted. */
static int io_wait( int sock, int forWrite ) {
    struct pollfd pfd;
    pfd.fd = sock;
    pfd.events = (short)( forWrite ? POLLOUT : POLLIN );
    pfd.revents = 0;
    return poll( &pfd, 1, 1000 );
    }
#endif


static int io_send_all( int sock, void *ssl, pthread_mutex_t *lock,
                        const unsigned char *buf, size_t len ) {
    size_t sent = 0;

#ifdef USE_TLS
    if( ssl != NULL ) {
        while( sent < len ) {
            int n, err;
            if( lock != NULL ) {
                pthread_mutex_lock( lock );
                }
            n = SSL_write( (SSL *)ssl, buf + sent, (int)( len - sent ) );
            err = ( n <= 0 ) ? SSL_get_error( (SSL *)ssl, n ) : 0;
            if( lock != NULL ) {
                pthread_mutex_unlock( lock );
                }
            if( n > 0 ) {
                sent += (size_t)n;
                continue;
                }
            if( err == SSL_ERROR_WANT_WRITE ) { io_wait( sock, 1 ); continue; }
            if( err == SSL_ERROR_WANT_READ )  { io_wait( sock, 0 ); continue; }
            return -1;
            }
        return 0;
        }
#else
    (void)ssl;
    (void)lock;
#endif

    while( sent < len ) {
        int n = (int)send( sock, buf + sent, len - sent, 0 );
        if( n <= 0 ) {
            if( n < 0 && errno == EINTR ) {
                continue;
                }
            return -1;
            }
        sent += (size_t)n;
        }
    return 0;
    }


static int io_recv_all( int sock, void *ssl, pthread_mutex_t *lock,
                        unsigned char *buf, size_t len ) {
    size_t got = 0;

#ifdef USE_TLS
    if( ssl != NULL ) {
        while( got < len ) {
            int n, err;
            if( lock != NULL ) {
                pthread_mutex_lock( lock );
                }
            n = SSL_read( (SSL *)ssl, buf + got, (int)( len - got ) );
            err = ( n <= 0 ) ? SSL_get_error( (SSL *)ssl, n ) : 0;
            if( lock != NULL ) {
                pthread_mutex_unlock( lock );
                }
            if( n > 0 ) {
                got += (size_t)n;
                continue;
                }
            if( err == SSL_ERROR_WANT_READ )  { io_wait( sock, 0 ); continue; }
            if( err == SSL_ERROR_WANT_WRITE ) { io_wait( sock, 1 ); continue; }
            return -1;
            }
        return 0;
        }
#else
    (void)ssl;
    (void)lock;
#endif

    while( got < len ) {
        int n = (int)recv( sock, buf + got, len - got, 0 );
        if( n <= 0 ) {
            if( n < 0 && errno == EINTR ) {
                continue;
                }
            return -1;
            }
        got += (size_t)n;
        }
    return 0;
    }


/* One read, returning as soon as any bytes arrive.  Mirrors recv()'s
   contract: >0 bytes, 0 on a clean close, <0 on error. */
static int io_recv_some( int sock, void *ssl, pthread_mutex_t *lock,
                         unsigned char *buf, size_t len ) {
#ifdef USE_TLS
    if( ssl != NULL ) {
        for( ;; ) {
            int n, err;
            if( lock != NULL ) {
                pthread_mutex_lock( lock );
                }
            n = SSL_read( (SSL *)ssl, buf, (int)len );
            err = ( n <= 0 ) ? SSL_get_error( (SSL *)ssl, n ) : 0;
            if( lock != NULL ) {
                pthread_mutex_unlock( lock );
                }
            if( n > 0 ) {
                return n;
                }
            if( err == SSL_ERROR_WANT_READ )  { io_wait( sock, 0 ); continue; }
            if( err == SSL_ERROR_WANT_WRITE ) { io_wait( sock, 1 ); continue; }
            return ( err == SSL_ERROR_ZERO_RETURN ) ? 0 : -1;
            }
        }
#else
    (void)ssl;
    (void)lock;
#endif
    return (int)recv( sock, buf, len, 0 );
    }


#ifdef USE_TLS
/* Promote a freshly accepted plain socket to TLS.  The fd is switched to
   non-blocking for the rest of its life, so the duplex relay never blocks
   one direction inside an SSL call.  A handshake that makes no progress
   for a while is abandoned rather than parking a thread forever (a port
   scanner or a plain-http probe hitting the TLS port must not leak
   threads).  Returns the SSL* or NULL on failure. */
static void *tls_accept( int sock ) {
    SSL *ssl;
    int flags;
    int idle = 0;

    flags = fcntl( sock, F_GETFL, 0 );
    if( flags >= 0 ) {
        fcntl( sock, F_SETFL, flags | O_NONBLOCK );
        }

    ssl = SSL_new( gCtx );
    if( ssl == NULL ) {
        return NULL;
        }
    SSL_set_fd( ssl, sock );

    for( ;; ) {
        int r = SSL_accept( ssl );
        int err;
        int forWrite;

        if( r == 1 ) {
            return ssl;
            }
        err = SSL_get_error( ssl, r );
        if( err == SSL_ERROR_WANT_READ ) {
            forWrite = 0;
            }
        else if( err == SSL_ERROR_WANT_WRITE ) {
            forWrite = 1;
            }
        else {
            SSL_free( ssl );
            return NULL;
            }
        if( io_wait( sock, forWrite ) == 0 ) {
            if( ++idle > 15 ) {
                SSL_free( ssl );
                return NULL;
                }
            }
        else {
            idle = 0;
            }
        }
    }
#endif


/* ------------------------------------------------------------------ */
/* Outgoing queue.  gLock must be held by the caller.                  */
/* ------------------------------------------------------------------ */

static int conn_queue( Conn *c, const unsigned char *data, size_t len,
                       int droppable ) {
    int dropped = 0;

    pthread_mutex_lock( &c->lock );

    if( c->closing ) {
        pthread_mutex_unlock( &c->lock );
        return 0;
        }

    if( droppable && c->outLen + len > OUT_LIMIT ) {
        /* This viewer cannot keep up.  Throw away its backlog rather than
           stalling everyone else, and force it back into sync later. */
        c->outLen = 0;
        c->inSync = 0;
        dropped = 1;
        }
    else {
        if( c->outLen + len > c->outCap ) {
            size_t want = c->outCap ? c->outCap : 65536;
            unsigned char *bigger;
            while( want < c->outLen + len ) {
                want *= 2;
                }
            bigger = (unsigned char *)realloc( c->out, want );
            if( bigger == NULL ) {
                c->outLen = 0;
                c->inSync = 0;
                dropped = 1;
                }
            else {
                c->out = bigger;
                c->outCap = want;
                }
            }
        if( !dropped ) {
            memcpy( c->out + c->outLen, data, len );
            c->outLen += len;
            }
        }

    pthread_cond_signal( &c->wake );
    pthread_mutex_unlock( &c->lock );

    return dropped;
    }


/* Build a server->client WebSocket frame (never masked) and queue it. */
static int ws_queue_frame( Conn *c, int opcode, const unsigned char *payload,
                           size_t len, int droppable ) {
    unsigned char header[10];
    size_t hlen = 0;
    unsigned char *frame;
    int result;

    header[0] = (unsigned char)( 0x80 | ( opcode & 0x0F ) );
    if( len < 126 ) {
        header[1] = (unsigned char)len;
        hlen = 2;
        }
    else if( len < 65536 ) {
        header[1] = 126;
        header[2] = (unsigned char)( ( len >> 8 ) & 0xFF );
        header[3] = (unsigned char)( len & 0xFF );
        hlen = 4;
        }
    else {
        /* Never more than 32 bits here, so the top four bytes are zero. */
        header[1] = 127;
        header[2] = 0;
        header[3] = 0;
        header[4] = 0;
        header[5] = 0;
        header[6] = (unsigned char)( ( (unsigned long)len >> 24 ) & 0xFF );
        header[7] = (unsigned char)( ( (unsigned long)len >> 16 ) & 0xFF );
        header[8] = (unsigned char)( ( (unsigned long)len >> 8 ) & 0xFF );
        header[9] = (unsigned char)( (unsigned long)len & 0xFF );
        hlen = 10;
        }

    frame = (unsigned char *)malloc( hlen + len );
    if( frame == NULL ) {
        return 1;
        }
    memcpy( frame, header, hlen );
    if( len > 0 ) {
        memcpy( frame + hlen, payload, len );
        }

    result = conn_queue( c, frame, hlen + len, droppable );
    free( frame );
    return result;
    }


static void ws_text( Conn *c, const char *text ) {
    ws_queue_frame( c, 0x1, (const unsigned char *)text, strlen( text ), 0 );
    }


/* ------------------------------------------------------------------ */
/* Relay logic.  All of these expect gLock to be held.                 */
/* ------------------------------------------------------------------ */

/* Timestamped one-line event log, so an operator can see exactly why a
   stream restarted instead of guessing. */
static void logmsg( const char *fmt, ... ) {
    va_list args;
    time_t t = time( NULL );
    struct tm *lt = localtime( &t );

    if( lt != NULL ) {
        printf( "[%02d:%02d:%02d] ", lt->tm_hour, lt->tm_min, lt->tm_sec );
        }
    va_start( args, fmt );
    vprintf( fmt, args );
    va_end( args );
    printf( "\n" );
    fflush( stdout );
    }


static Conn *find_conn( int id ) {
    int i;
    if( id == 0 ) {
        return NULL;
        }
    for( i = 0; i < MAX_CONNS; i++ ) {
        if( gConns[i].used && gConns[i].id == id ) {
            return &gConns[i];
            }
        }
    return NULL;
    }


static void broadcast_count( void ) {
    char msg[64];
    int count = 0;
    int i;

    for( i = 0; i < MAX_CONNS; i++ ) {
        if( gConns[i].used && !gConns[i].closing ) {
            count++;
            }
        }
    sprintf( msg, "COUNT %d", count );
    for( i = 0; i < MAX_CONNS; i++ ) {
        if( gConns[i].used ) {
            ws_text( &gConns[i], msg );
            }
        }
    }


static void broadcast_idle( void ) {
    int i;
    for( i = 0; i < MAX_CONNS; i++ ) {
        if( gConns[i].used ) {
            gConns[i].inSync = 0;
            ws_text( &gConns[i], "IDLE" );
            }
        }
    }


/*
 * Ask the current sharer to begin a brand new stream generation.  Every
 * viewer will be handed a fresh init segment, so anyone who just joined
 * (or fell out of sync) lands on a clean keyframe instead of trying to
 * splice into the middle of a cluster.
 */
static void request_restart( const char *why ) {
    Conn *s;
    if( gSharerId == 0 || gAwaitStart ) {
        return;
        }
    s = find_conn( gSharerId );
    if( s == NULL ) {
        gSharerId = 0;
        return;
        }
    gAwaitStart = 1;
    gAwaitSince = time( NULL );
    logmsg( "restarting stream (%s)", why );
    ws_text( s, "RESTART" );
    }


/*
 * A sharer that never answers a RESTART would otherwise block every
 * future joiner forever, so give up waiting and ask again.
 */
static void expire_pending_restart( void ) {
    if( !gAwaitStart ) {
        return;
        }
    if( time( NULL ) - gAwaitSince < 5 ) {
        return;
        }
    gAwaitStart = 0;
    request_restart( "sharer never answered" );
    }


static void relay_media( Conn *from, const unsigned char *data, size_t len ) {
    int i;
    int anyDropped = 0;

    if( gSharerId != from->id ) {
        return;   /* stale blocks from a demoted sharer, ignore */
        }

    for( i = 0; i < MAX_CONNS; i++ ) {
        Conn *c = &gConns[i];
        if( !c->used || c->id == from->id || !c->inSync ) {
            continue;
            }
        if( ws_queue_frame( c, 0x2, data, len, 1 ) ) {
            anyDropped = 1;
            }
        }

    if( anyDropped ) {
        request_restart( "viewer fell behind" );
        }
    }


static void handle_text( Conn *c, const char *msg ) {
    if( strncmp( msg, "SHARE ", 6 ) == 0 ) {
        int i;
        Conn *old = find_conn( gSharerId );

        if( old != NULL && old->id != c->id ) {
            old->isSharer = 0;
            ws_text( old, "STOPPED" );
            }

        strncpy( gMime, msg + 6, MIME_MAX - 1 );
        gMime[ MIME_MAX - 1 ] = '\0';

        logmsg( "client %d is now sharing (%s)", c->id, gMime );
        c->isSharer = 1;
        c->inSync = 0;
        gSharerId = c->id;
        gAwaitStart = 0;

        for( i = 0; i < MAX_CONNS; i++ ) {
            if( gConns[i].used ) {
                gConns[i].inSync = 0;
                }
            }
        request_restart( "new sharer" );
        }
    else if( strcmp( msg, "START" ) == 0 ) {
        /* The sharer's next binary block opens a new stream. */
        if( gSharerId == c->id ) {
            char reset[ MIME_MAX + 16 ];
            int i;
            gAwaitStart = 0;
            sprintf( reset, "RESET %s", gMime );
            for( i = 0; i < MAX_CONNS; i++ ) {
                Conn *v = &gConns[i];
                if( !v->used || v->id == c->id ) {
                    continue;
                    }
                v->outLen = 0;   /* nothing older matters any more */
                v->inSync = 1;
                ws_text( v, reset );
                }
            }
        }
    else if( strcmp( msg, "STOP" ) == 0 ) {
        if( gSharerId == c->id ) {
            logmsg( "client %d stopped sharing", c->id );
            gSharerId = 0;
            gAwaitStart = 0;
            c->isSharer = 0;
            broadcast_idle();
            }
        }
    else if( strcmp( msg, "HELLO" ) == 0 ) {
        /* The join already broadcast the new count; just report the
           sharing state to this one newcomer. */
        if( gSharerId != 0 ) {
            request_restart( "viewer joined" );
            }
        else {
            ws_text( c, "IDLE" );
            }
        }
    /* anything else is a heartbeat and needs no reply */
    }


/* ------------------------------------------------------------------ */
/* Writer thread: drains one connection's queue                        */
/* ------------------------------------------------------------------ */

static void *writer_thread( void *arg ) {
    Conn *c = (Conn *)arg;

    for( ;; ) {
        unsigned char *chunk;
        size_t chunkLen;

        pthread_mutex_lock( &c->lock );
        while( c->outLen == 0 && !c->closing ) {
            pthread_cond_wait( &c->wake, &c->lock );
            }
        if( c->outLen == 0 && c->closing ) {
            pthread_mutex_unlock( &c->lock );
            break;
            }
        chunk = c->out;
        chunkLen = c->outLen;
        c->out = NULL;
        c->outLen = 0;
        c->outCap = 0;
        pthread_mutex_unlock( &c->lock );

        if( io_send_all( c->sock, c->ssl, &c->sslLock, chunk, chunkLen )
            != 0 ) {
            free( chunk );
            pthread_mutex_lock( &c->lock );
            c->closing = 1;
            pthread_mutex_unlock( &c->lock );
            shutdown( c->sock, SHUT_RDWR );
            break;
            }
        free( chunk );
        }

    return NULL;
    }


/* ------------------------------------------------------------------ */
/* WebSocket handshake                                                 */
/* ------------------------------------------------------------------ */

/* Runs in the reader thread before the writer exists, so there is no
   concurrent SSL access yet and the I/O calls pass a NULL lock. */
static int ws_handshake( Conn *c ) {
    int sock = c->sock;
    char req[4096];
    int total = 0;
    char *keyStart;
    char *keyEnd;
    char key[256];
    char combined[320];
    unsigned char digest[20];
    char accept[64];
    char response[512];
    Sha1 ctx;
    int keyLen;

    for( ;; ) {
        int n = io_recv_some( sock, c->ssl, NULL,
                              (unsigned char *)( req + total ),
                              sizeof( req ) - 1 - total );
        if( n <= 0 ) {
            return -1;
            }
        total += n;
        req[total] = '\0';
        if( strstr( req, "\r\n\r\n" ) != NULL ) {
            break;
            }
        if( total >= (int)sizeof( req ) - 1 ) {
            return -1;
            }
        }

    /* The relay port is gated too, otherwise the secret URL only hides   */
    /* the page and anybody could still tap the video stream directly.    */
    if( !path_authorized( req ) ) {
        static const char *deny =
            "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        io_send_all( sock, c->ssl, NULL,
                     (const unsigned char *)deny, strlen( deny ) );
        return -1;
        }

    keyStart = strstr( req, "Sec-WebSocket-Key:" );
    if( keyStart == NULL ) {
        keyStart = strstr( req, "sec-websocket-key:" );
        }
    if( keyStart == NULL ) {
        return -1;
        }
    keyStart += 18;
    while( *keyStart == ' ' ) {
        keyStart++;
        }
    keyEnd = strstr( keyStart, "\r\n" );
    if( keyEnd == NULL ) {
        return -1;
        }
    keyLen = (int)( keyEnd - keyStart );
    if( keyLen <= 0 || keyLen >= (int)sizeof( key ) ) {
        return -1;
        }
    memcpy( key, keyStart, keyLen );
    key[keyLen] = '\0';

    strcpy( combined, key );
    strcat( combined, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" );

    sha1_init( &ctx );
    sha1_update( &ctx, (const unsigned char *)combined,
                 (unsigned int)strlen( combined ) );
    sha1_final( &ctx, digest );
    base64_encode( digest, 20, accept );

    sprintf( response,
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n", accept );

    return io_send_all( sock, c->ssl, NULL,
                        (const unsigned char *)response, strlen( response ) );
    }


/* ------------------------------------------------------------------ */
/* Reader thread: owns a connection's whole lifetime                   */
/* ------------------------------------------------------------------ */

static void conn_teardown( Conn *c ) {
    pthread_mutex_lock( &c->lock );
    c->closing = 1;
    pthread_cond_signal( &c->wake );
    pthread_mutex_unlock( &c->lock );

    shutdown( c->sock, SHUT_RDWR );
    pthread_join( c->writer, NULL );

    pthread_mutex_lock( &gLock );
    if( gSharerId == c->id ) {
        logmsg( "sharer (client %d) disconnected", c->id );
        gSharerId = 0;
        gAwaitStart = 0;
        broadcast_idle();
        }
    logmsg( "client %d disconnected", c->id );
#ifdef USE_TLS
    if( c->ssl != NULL ) {
        SSL_free( (SSL *)c->ssl );
        c->ssl = NULL;
        }
#endif
    close( c->sock );
    free( c->out );
    c->out = NULL;
    c->outLen = 0;
    c->outCap = 0;
    /* Destroy before releasing the slot: the moment used goes to 0 the
       accept loop may claim this slot and re-init these primitives. */
    pthread_mutex_destroy( &c->lock );
    pthread_cond_destroy( &c->wake );
    pthread_mutex_destroy( &c->sslLock );
    c->used = 0;
    broadcast_count();
    pthread_mutex_unlock( &gLock );
    }


static void *reader_thread( void *arg ) {
    Conn *c = (Conn *)arg;
    unsigned char *message = NULL;
    size_t messageLen = 0;
    size_t messageCap = 0;
    int messageOp = 0;

#ifdef USE_TLS
    if( gCtx != NULL ) {
        c->ssl = tls_accept( c->sock );
        if( c->ssl == NULL ) {
            close( c->sock );
            pthread_mutex_lock( &gLock );
            pthread_mutex_destroy( &c->lock );
            pthread_cond_destroy( &c->wake );
            pthread_mutex_destroy( &c->sslLock );
            c->used = 0;
            pthread_mutex_unlock( &gLock );
            return NULL;
            }
        }
#endif

    if( ws_handshake( c ) != 0 ||
        pthread_create( &c->writer, NULL, writer_thread, c ) != 0 ) {
#ifdef USE_TLS
        if( c->ssl != NULL ) {
            SSL_free( (SSL *)c->ssl );
            c->ssl = NULL;
            }
#endif
        close( c->sock );
        pthread_mutex_lock( &gLock );
        pthread_mutex_destroy( &c->lock );
        pthread_cond_destroy( &c->wake );
        pthread_mutex_destroy( &c->sslLock );
        c->used = 0;
        pthread_mutex_unlock( &gLock );
        return NULL;
        }

    pthread_mutex_lock( &gLock );
    logmsg( "client %d connected", c->id );
    broadcast_count();
    pthread_mutex_unlock( &gLock );

    for( ;; ) {
        unsigned char head[2];
        unsigned char ext[8];
        unsigned char mask[4];
        unsigned long payloadLen;
        int fin, opcode, masked;
        unsigned char *payload = NULL;
        unsigned long i;

        if( io_recv_all( c->sock, c->ssl, &c->sslLock, head, 2 ) != 0 ) {
            break;
            }
        fin = ( head[0] & 0x80 ) ? 1 : 0;
        opcode = head[0] & 0x0F;
        masked = ( head[1] & 0x80 ) ? 1 : 0;
        payloadLen = (unsigned long)( head[1] & 0x7F );

        if( payloadLen == 126 ) {
            if( io_recv_all( c->sock, c->ssl, &c->sslLock, ext, 2 ) != 0 ) {
                break;
                }
            payloadLen = ( (unsigned long)ext[0] << 8 ) | ext[1];
            }
        else if( payloadLen == 127 ) {
            if( io_recv_all( c->sock, c->ssl, &c->sslLock, ext, 8 ) != 0 ) {
                break;
                }
            if( ext[0] || ext[1] || ext[2] || ext[3] ) {
                break;   /* absurdly large, drop the client */
                }
            payloadLen = ( (unsigned long)ext[4] << 24 ) |
                         ( (unsigned long)ext[5] << 16 ) |
                         ( (unsigned long)ext[6] << 8 ) |
                         (unsigned long)ext[7];
            }

        if( payloadLen > MAX_MESSAGE ) {
            break;
            }
        if( masked ) {
            if( io_recv_all( c->sock, c->ssl, &c->sslLock, mask, 4 ) != 0 ) {
                break;
                }
            }

        if( payloadLen > 0 ) {
            payload = (unsigned char *)malloc( payloadLen );
            if( payload == NULL ) {
                break;
                }
            if( io_recv_all( c->sock, c->ssl, &c->sslLock, payload, payloadLen ) != 0 ) {
                free( payload );
                break;
                }
            if( masked ) {
                for( i = 0; i < payloadLen; i++ ) {
                    payload[i] ^= mask[ i & 3 ];
                    }
                }
            }

        c->lastSeen = time( NULL );

        if( opcode == 0x8 ) {
            free( payload );
            break;
            }
        else if( opcode == 0x9 ) {
            pthread_mutex_lock( &gLock );
            ws_queue_frame( c, 0xA, payload, payloadLen, 0 );
            pthread_mutex_unlock( &gLock );
            free( payload );
            continue;
            }
        else if( opcode == 0xA ) {
            free( payload );
            continue;
            }

        /* data frame, possibly fragmented */
        if( opcode != 0x0 ) {
            messageLen = 0;
            messageOp = opcode;
            }
        if( messageLen + payloadLen > MAX_MESSAGE ) {
            free( payload );
            break;
            }
        if( payloadLen > 0 ) {
            if( messageLen + payloadLen > messageCap ) {
                size_t want = messageCap ? messageCap : 65536;
                unsigned char *bigger;
                while( want < messageLen + payloadLen ) {
                    want *= 2;
                    }
                bigger = (unsigned char *)realloc( message, want );
                if( bigger == NULL ) {
                    free( payload );
                    break;
                    }
                message = bigger;
                messageCap = want;
                }
            memcpy( message + messageLen, payload, payloadLen );
            messageLen += payloadLen;
            }
        free( payload );

        if( !fin ) {
            continue;
            }

        if( messageOp == 0x1 ) {
            char *text = (char *)malloc( messageLen + 1 );
            if( text != NULL ) {
                if( messageLen > 0 ) {
                    memcpy( text, message, messageLen );
                    }
                text[messageLen] = '\0';
                pthread_mutex_lock( &gLock );
                handle_text( c, text );
                pthread_mutex_unlock( &gLock );
                free( text );
                }
            }
        else if( messageOp == 0x2 && messageLen > 0 ) {
            pthread_mutex_lock( &gLock );
            relay_media( c, message, messageLen );
            pthread_mutex_unlock( &gLock );
            }
        messageLen = 0;
        }

    free( message );
    conn_teardown( c );
    return NULL;
    }


/* ------------------------------------------------------------------ */
/* Heartbeat: keeps NAT tables warm and reaps corpses                  */
/* ------------------------------------------------------------------ */

static void *heartbeat_thread( void *arg ) {
    (void)arg;
    for( ;; ) {
        int i;
        time_t now;

        sleep( PING_SECONDS );
        now = time( NULL );

        pthread_mutex_lock( &gLock );
        expire_pending_restart();
        for( i = 0; i < MAX_CONNS; i++ ) {
            Conn *c = &gConns[i];
            if( !c->used || c->closing ) {
                continue;
                }
            if( now - c->lastSeen > DEAD_SECONDS ) {
                pthread_mutex_lock( &c->lock );
                c->closing = 1;
                pthread_cond_signal( &c->wake );
                pthread_mutex_unlock( &c->lock );
                shutdown( c->sock, SHUT_RDWR );
                continue;
                }
            ws_queue_frame( c, 0x9, NULL, 0, 0 );
            }
        pthread_mutex_unlock( &gLock );
        }
    return NULL;
    }


/* ------------------------------------------------------------------ */
/* The browser client                                                  */
/* ------------------------------------------------------------------ */

static const char *gPage[] = {
"<!doctype html>\n",
"<meta charset='utf-8'>\n",
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n",
"<title>cscreen</title>\n",
"<style>\n",
"html,body{margin:0;height:100%;background:#111;color:#ddd;\n",
" font:14px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;}\n",
"body{display:flex;flex-direction:column;overflow:hidden;}\n",
"#bar{display:flex;align-items:center;gap:10px;padding:8px 12px;\n",
" background:#1c1c1e;border-bottom:1px solid #000;flex:0 0 auto;\n",
" flex-wrap:wrap;}\n",
"button{background:#2f6fd0;color:#fff;border:0;border-radius:5px;\n",
" padding:7px 14px;font-size:14px;cursor:pointer;}\n",
"button:hover{background:#3a80e8;}\n",
"button:disabled{background:#3a3a3d;color:#777;cursor:default;}\n",
"button.stop{background:#c0392b;}\n",
"button.stop:hover{background:#d84a3a;}\n",
"button.plain{background:#3a3a3d;}\n",
"button.plain:hover{background:#4a4a4f;}\n",
"select{background:#2a2a2d;color:#ddd;border:1px solid #444;\n",
" border-radius:5px;padding:6px;font-size:14px;}\n",
"label{color:#999;}\n",
".spacer{flex:1 1 auto;}\n",
"#count{color:#8ab4f8;font-variant-numeric:tabular-nums;}\n",
"#status{color:#999;}\n",
"#dot{display:inline-block;width:9px;height:9px;border-radius:50%;\n",
" background:#c0392b;margin-right:6px;vertical-align:middle;}\n",
"#dot.on{background:#27ae60;}\n",
"#stage{flex:1 1 auto;position:relative;background:#000;min-height:0;}\n",
"video{width:100%;height:100%;object-fit:contain;background:#000;\n",
" display:block;}\n",
"#msg{position:absolute;left:0;right:0;top:50%;transform:translateY(-50%);\n",
" text-align:center;color:#777;font-size:16px;pointer-events:none;}\n",
"</style>\n",
"<div id='bar'>\n",
" <button id='shareBtn'>Share My Screen</button>\n",
" <label for='fps'>fps</label>\n",
" <select id='fps'>\n",
"  <option value='60'>60</option>\n",
"  <option value='30' selected>30</option>\n",
"  <option value='15'>15</option>\n",
"  <option value='10'>10</option>\n",
"  <option value='5'>5</option>\n",
" </select>\n",
" <button id='stopBtn' class='stop' disabled>Stop Sharing</button>\n",
" <span class='spacer'></span>\n",
" <span id='status'><span id='dot'></span><span id='statusText'>connecting</span></span>\n",
" <span id='count'>1 connected</span>\n",
" <button id='fsBtn' class='plain'>Fullscreen</button>\n",
"</div>\n",
"<div id='stage'>\n",
" <video id='vid' autoplay muted playsinline></video>\n",
" <div id='msg'>Connecting to relay...</div>\n",
"</div>\n",
"<script>\n",
"@@PORT@@",
"var CANDIDATES=['video/webm;codecs=vp8','video/webm;codecs=vp9',\n",
"                'video/webm;codecs=h264','video/webm'];\n",
"var TIMESLICE=100;\n",
"var BITRATE=6000000;\n",
"var BACKLOG_MAX=8000000;\n",
"\n",
"var vid=document.getElementById('vid');\n",
"var msg=document.getElementById('msg');\n",
"var dot=document.getElementById('dot');\n",
"var statusEl=document.getElementById('statusText');\n",
"var countEl=document.getElementById('count');\n",
"var shareBtn=document.getElementById('shareBtn');\n",
"var stopBtn=document.getElementById('stopBtn');\n",
"var fpsSel=document.getElementById('fps');\n",
"var fsBtn=document.getElementById('fsBtn');\n",
"\n",
"var ws=null;\n",
"var wantShare=false;\n",
"var stream=null;\n",
"var rec=null;\n",
"var mime='';\n",
"var sendQ=Promise.resolve();\n",
"var ms=null,sb=null,msUrl=null;\n",
"var queue=[];\n",
"var pendingMime='';\n",
"var lastMedia=0;\n",
"var lastRecChunk=0;\n",
"var lastRestart=0;\n",
"var lastEnd=-1;\n",
"var lastEndTime=0;\n",
"var lastCT=-1;\n",
"var lastCTTime=0;\n",
"var serverSharing=false;\n",
"var statusText='connecting';\n",
"var lastResync='none';\n",
"var lastSeek=0;\n",
"var resyncCount=0;\n",
"\n",
"function now(){return Date.now();}\n",
"\n",
"function pickMime(){\n",
" var i;\n",
" if(!window.MediaRecorder)return '';\n",
" for(i=0;i<CANDIDATES.length;i++){\n",
"  if(MediaRecorder.isTypeSupported(CANDIDATES[i]))return CANDIDATES[i];\n",
" }\n",
" return '';\n",
"}\n",
"\n",
"function setStatus(t,ok){\n",
" statusText=t;\n",
" statusEl.textContent=t;\n",
" dot.className=ok?'on':'';\n",
"}\n",
"\n",
"function showMsg(t){\n",
" if(t){msg.textContent=t;msg.style.display='block';}\n",
" else msg.style.display='none';\n",
"}\n",
"\n",
"/* ---------------- relay socket ---------------- */\n",
"\n",
"function connect(){\n",
"/* The relay port demands the same access code the page was served\n",
"   under, so just carry our own path over to the socket URL.  When the\n",
"   page itself came over https the browser forbids a plain ws:// socket\n",
"   (mixed content), so match the page's scheme: https -> wss. */\n",
" var scheme=(location.protocol==='https:')?'wss://':'ws://';\n",
" var url=scheme+location.hostname+':'+WSPORT+location.pathname;\n",
" try{ws=new WebSocket(url);}catch(e){setTimeout(connect,500);return;}\n",
" ws.binaryType='arraybuffer';\n",
" ws.onopen=function(){\n",
"  setStatus(wantShare?'sharing':'connected',true);\n",
"  send('HELLO');\n",
"  if(wantShare&&stream)send('SHARE '+mime);\n",
" };\n",
" ws.onmessage=function(ev){\n",
"  if(typeof ev.data==='string')onControl(ev.data);\n",
"  else onMedia(ev.data);\n",
" };\n",
" ws.onerror=function(){try{ws.close();}catch(e){}};\n",
" ws.onclose=function(){\n",
"  ws=null;\n",
"  setStatus('reconnecting',false);\n",
"  if(!wantShare)showMsg('Reconnecting to relay...');\n",
"  setTimeout(connect,400);\n",
" };\n",
"}\n",
"\n",
"function send(t){\n",
" if(ws&&ws.readyState===1){try{ws.send(t);}catch(e){}}\n",
"}\n",
"\n",
"function onControl(m){\n",
" if(m.indexOf('COUNT ')===0){\n",
"  var n=parseInt(m.substring(6),10);\n",
"  countEl.textContent=n+(n===1?' person':' people')+' connected';\n",
"  return;\n",
" }\n",
" if(m==='RESTART'){ if(wantShare)restartRecorder(); return; }\n",
" if(m==='STOPPED'){ localStop(true); return; }\n",
" if(m.indexOf('RESET ')===0){\n",
"  serverSharing=true;\n",
"  pendingMime=m.substring(6);\n",
"  resetPlayer(pendingMime);\n",
"  return;\n",
" }\n",
" if(m==='IDLE'){\n",
"  serverSharing=false;\n",
"  teardownPlayer();\n",
"  if(!wantShare)showMsg('Nobody is sharing yet.');\n",
"  return;\n",
" }\n",
"}\n",
"\n",
"/* ---------------- viewer side ---------------- */\n",
"\n",
"function teardownPlayer(){\n",
" queue=[];\n",
" sb=null;\n",
" if(ms){try{if(ms.readyState==='open')ms.endOfStream();}catch(e){}}\n",
" ms=null;\n",
" if(msUrl){try{URL.revokeObjectURL(msUrl);}catch(e){}msUrl=null;}\n",
" if(!wantShare){try{vid.removeAttribute('src');vid.load();}catch(e){}}\n",
"}\n",
"\n",
"function resetPlayer(m){\n",
" if(wantShare)return;\n",
" teardownPlayer();\n",
" if(!window.MediaSource||!MediaSource.isTypeSupported(m)){\n",
"  showMsg('This browser cannot play '+m);\n",
"  return;\n",
" }\n",
" showMsg('Starting stream...');\n",
" ms=new MediaSource();\n",
" msUrl=URL.createObjectURL(ms);\n",
" vid.src=msUrl;\n",
" var mine=ms;\n",
" ms.addEventListener('sourceopen',function(){\n",
"  if(ms!==mine)return;\n",
"  try{\n",
"   sb=ms.addSourceBuffer(m);\n",
"   sb.mode='sequence';\n",
"  }catch(e){ showMsg('Cannot decode stream: '+e); return; }\n",
"  sb.addEventListener('updateend',pump);\n",
"  sb.addEventListener('error',function(){forceResync('sourcebuffer error');});\n",
"  pump();\n",
" });\n",
" lastEnd=-1;\n",
" lastEndTime=now();\n",
" lastCT=-1;\n",
" lastCTTime=now();\n",
"}\n",
"\n",
"function onMedia(buf){\n",
" lastMedia=now();\n",
" if(wantShare)return;\n",
" queue.push(buf);\n",
" /* Hopelessly behind (or the decoder never opened).  Dropping blocks\n",
"    here would splice corruption in, so start over cleanly instead. */\n",
" if(queue.length>200){forceResync('backlog');return;}\n",
" pump();\n",
"}\n",
"\n",
"function pump(){\n",
" if(!sb||sb.updating)return;\n",
" if(queue.length===0){trim();return;}\n",
" var chunk=queue.shift();\n",
" try{\n",
"  sb.appendBuffer(chunk);\n",
"  showMsg('');\n",
"  /* Drive playback from arriving data, not from a timer: background\n",
"     tabs throttle setInterval to about once a minute. */\n",
"  ensurePlaying();\n",
"  chase();\n",
" }catch(e){\n",
"  if(e&&e.name==='QuotaExceededError'){\n",
"   queue.unshift(chunk);\n",
"   evict();\n",
"  }\n",
"  else forceResync('append failed: '+(e&&e.name));\n",
" }\n",
"}\n",
"\n",
"function evict(){\n",
" try{\n",
"  if(sb&&!sb.updating&&vid.buffered.length){\n",
"   var s=vid.buffered.start(0);\n",
"   var e=Math.max(s+0.5,vid.currentTime-1);\n",
"   if(e>s)sb.remove(s,e);\n",
"  }\n",
" }catch(x){}\n",
"}\n",
"\n",
"function trim(){\n",
" try{\n",
"  if(sb&&!sb.updating&&vid.buffered.length){\n",
"   var s=vid.buffered.start(0);\n",
"   if(vid.currentTime-s>10)sb.remove(s,vid.currentTime-5);\n",
"  }\n",
" }catch(x){}\n",
"}\n",
"\n",
"/* A paused element still lets its buffer grow, so the stream looks\n",
"   healthy while the picture is frozen.  Never assume autoplay took. */\n",
"function ensurePlaying(){\n",
" if(wantShare||!vid.paused)return;\n",
" try{\n",
"  var p=vid.play();\n",
"  if(p&&p['catch'])p['catch'](function(){});\n",
" }catch(e){}\n",
"}\n",
"\n",
"/* Chase the live edge: screen sharing is worthless if it lags behind. */\n",
"function chase(){\n",
" if(wantShare||!vid.buffered||!vid.buffered.length)return;\n",
" var end=vid.buffered.end(vid.buffered.length-1);\n",
" var lag=end-vid.currentTime;\n",
" if(lag>2||lag<0){\n",
"  /* Seeking flushes the decoder, so never do it back to back: under\n",
"     load that alone can keep playback from ever making progress. */\n",
"  if(now()-lastSeek>1000){\n",
"   lastSeek=now();\n",
"   try{vid.currentTime=Math.max(0,end-0.1);}catch(e){}\n",
"  }\n",
"  vid.playbackRate=1;\n",
" }\n",
" else if(lag>0.5)vid.playbackRate=1.15;\n",
" else vid.playbackRate=1;\n",
" ensurePlaying();\n",
"}\n",
"\n",
"/* Something is wedged.  Bounce the socket; the relay hands us a fresh\n",
"   stream the moment we come back. */\n",
"function forceResync(why){\n",
" lastResync=why+' @'+new Date().toLocaleTimeString();\n",
" resyncCount++;\n",
" if(window.console)console.log('cscreen resync: '+why);\n",
" teardownPlayer();\n",
" if(ws){try{ws.close();}catch(e){}}\n",
"}\n",
"\n",
"/* ---------------- sharer side ---------------- */\n",
"\n",
"function startShare(){\n",
" var fps=parseInt(fpsSel.value,10);\n",
" mime=pickMime();\n",
" if(!mime){alert('This browser cannot record video.');return;}\n",
" if(!navigator.mediaDevices||!navigator.mediaDevices.getDisplayMedia){\n",
"  alert('This browser cannot capture the screen.');return;\n",
" }\n",
" navigator.mediaDevices.getDisplayMedia(\n",
"  {video:{frameRate:{ideal:fps,max:fps}},audio:false}\n",
" ).then(function(s){\n",
"  teardownPlayer();\n",
"  stream=s;\n",
"  wantShare=true;\n",
"  s.getVideoTracks()[0].addEventListener('ended',function(){localStop(false);});\n",
"  vid.srcObject=s;\n",
"  vid.play().catch(function(){});\n",
"  showMsg('');\n",
"  shareBtn.disabled=true;\n",
"  stopBtn.disabled=false;\n",
"  fpsSel.disabled=true;\n",
"  setStatus('sharing',true);\n",
"  send('SHARE '+mime);\n",
" }).catch(function(){});\n",
"}\n",
"\n",
"function restartRecorder(){\n",
" if(!wantShare||!stream)return;\n",
" lastRestart=now();\n",
" stopRecorder();\n",
" sendQ=sendQ.then(function(){send('START');});\n",
" var opts={mimeType:mime,videoBitsPerSecond:BITRATE};\n",
" try{rec=new MediaRecorder(stream,opts);}\n",
" catch(e){\n",
"  try{rec=new MediaRecorder(stream);}catch(e2){return;}\n",
" }\n",
" rec.ondataavailable=function(ev){\n",
"  if(!ev.data||ev.data.size===0)return;\n",
"  lastRecChunk=now();\n",
"  var b=ev.data;\n",
"  sendQ=sendQ.then(function(){return b.arrayBuffer();})\n",
"             .then(function(ab){\n",
"    if(!ws||ws.readyState!==1)return;\n",
"    /* Uplink is congested.  Skipping a block would corrupt the stream\n",
"       for every viewer, so cut a clean new generation instead. */\n",
"    if(ws.bufferedAmount>BACKLOG_MAX){\n",
"     if(now()-lastRestart>2000)restartRecorder();\n",
"     return;\n",
"    }\n",
"    try{ws.send(ab);}catch(e){}\n",
"   }).catch(function(){});\n",
" };\n",
" rec.onerror=function(){setTimeout(restartRecorder,300);};\n",
" try{rec.start(TIMESLICE);}catch(e){setTimeout(restartRecorder,300);return;}\n",
" lastRecChunk=now();\n",
"}\n",
"\n",
"function stopRecorder(){\n",
" if(rec){\n",
"  try{rec.ondataavailable=null;rec.onerror=null;}catch(e){}\n",
"  try{if(rec.state!=='inactive')rec.stop();}catch(e){}\n",
"  rec=null;\n",
" }\n",
"}\n",
"\n",
"/* Stop sharing.  takenOver means the relay gave the floor to someone\n",
"   else, so we stay connected and become a viewer. */\n",
"function localStop(takenOver){\n",
" var wasSharing=wantShare;\n",
" wantShare=false;\n",
" stopRecorder();\n",
" if(stream){\n",
"  var tracks=stream.getTracks();\n",
"  for(var i=0;i<tracks.length;i++){try{tracks[i].stop();}catch(e){}}\n",
"  stream=null;\n",
" }\n",
" try{vid.srcObject=null;}catch(e){}\n",
" shareBtn.disabled=false;\n",
" stopBtn.disabled=true;\n",
" fpsSel.disabled=false;\n",
" setStatus(ws?'connected':'reconnecting',ws?true:false);\n",
" if(!takenOver&&wasSharing)send('STOP');\n",
" if(takenOver)showMsg('Someone else started sharing.');\n",
" else showMsg('Nobody is sharing yet.');\n",
"}\n",
"\n",
"/* ---------------- watchdogs ---------------- */\n",
"\n",
"setInterval(function(){\n",
" send('P');\n",
"\n",
" if(wantShare){\n",
"  if(rec&&rec.state!=='recording'){restartRecorder();return;}\n",
"  if(!rec){restartRecorder();return;}\n",
"  if(now()-lastRecChunk>4000){restartRecorder();return;}\n",
"  if(stream){\n",
"   var t=stream.getVideoTracks()[0];\n",
"   if(!t||t.readyState==='ended'){localStop(false);}\n",
"  }\n",
"  return;\n",
" }\n",
"\n",
" if(serverSharing&&ws&&ws.readyState===1){\n",
"  /* nothing arriving at all: the relay path is wedged */\n",
"  if(lastMedia&&now()-lastMedia>6000){forceResync('no media 6s');return;}\n",
"  if(vid.buffered&&vid.buffered.length){\n",
"   var end=vid.buffered.end(vid.buffered.length-1);\n",
"   if(end>lastEnd+0.05){lastEnd=end;lastEndTime=now();}\n",
"   else if(now()-lastEndTime>6000){forceResync('buffer stalled 6s');return;}\n",
"   /* Data is flowing but the picture is frozen.  Nudge it first; if it\n",
"      stays stuck, tear the whole thing down and start clean. */\n",
"   if(vid.currentTime>lastCT+0.02){lastCT=vid.currentTime;lastCTTime=now();}\n",
"   else if(now()-lastCTTime>3000){\n",
"    ensurePlaying();\n",
"    chase();\n",
"    if(now()-lastCTTime>9000){forceResync('playback stalled 9s');return;}\n",
"   }\n",
"  }\n",
" }\n",
"},1000);\n",
"\n",
"/* Coming back to a throttled background tab: jump straight to live. */\n",
"document.addEventListener('visibilitychange',function(){\n",
" if(!document.hidden){ensurePlaying();chase();}\n",
"});\n",
"\n",
"setInterval(chase,250);\n",
"\n",
"/* ---------------- wiring ---------------- */\n",
"\n",
"shareBtn.onclick=startShare;\n",
"stopBtn.onclick=function(){localStop(false);};\n",
"fsBtn.onclick=function(){\n",
" var st=document.getElementById('stage');\n",
" if(document.fullscreenElement)document.exitFullscreen();\n",
" else if(st.requestFullscreen)st.requestFullscreen();\n",
"};\n",
"vid.addEventListener('click',ensurePlaying);\n",
"vid.addEventListener('canplay',ensurePlaying);\n",
"vid.addEventListener('waiting',chase);\n",
"vid.addEventListener('stalled',chase);\n",
"/* If anything ever pauses us, get straight back to playing. */\n",
"vid.addEventListener('pause',function(){\n",
" if(!wantShare&&sb)setTimeout(ensurePlaying,50);\n",
"});\n",
"\n",
"connect();\n",
"</script>\n",
0 };


/* Single-threaded per request, so all the I/O passes a NULL SSL lock. */
static void http_serve( int sock, void *ssl ) {
    char portLine[64];
    char header[256];
    size_t total = 0;
    int i;

    sprintf( portLine, "var WSPORT=%d;\n", gRelayPort );

    for( i = 0; gPage[i] != 0; i++ ) {
        if( strcmp( gPage[i], "@@PORT@@" ) == 0 ) {
            total += strlen( portLine );
            }
        else {
            total += strlen( gPage[i] );
            }
        }

    sprintf( header,
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html; charset=utf-8\r\n"
             "Content-Length: %lu\r\n"
             "Cache-Control: no-store\r\n"
             "Connection: close\r\n\r\n",
             (unsigned long)total );

    if( io_send_all( sock, ssl, NULL, (const unsigned char *)header,
                     strlen( header ) ) != 0 ) {
        return;
        }

    for( i = 0; gPage[i] != 0; i++ ) {
        const char *line = gPage[i];
        if( strcmp( line, "@@PORT@@" ) == 0 ) {
            line = portLine;
            }
        if( io_send_all( sock, ssl, NULL, (const unsigned char *)line,
                         strlen( line ) ) != 0 ) {
            return;
            }
        }
    }


static void *http_conn_thread( void *arg ) {
    int sock = *(int *)arg;
    char req[2048];
    int total = 0;
    void *ssl = NULL;

    free( arg );

#ifdef USE_TLS
    if( gCtx != NULL ) {
        ssl = tls_accept( sock );
        if( ssl == NULL ) {
            close( sock );
            return NULL;
            }
        }
#endif

    for( ;; ) {
        int n = io_recv_some( sock, ssl, NULL,
                              (unsigned char *)( req + total ),
                              sizeof( req ) - 1 - total );
        if( n <= 0 ) {
#ifdef USE_TLS
            if( ssl != NULL ) {
                SSL_free( (SSL *)ssl );
                }
#endif
            close( sock );
            return NULL;
            }
        total += n;
        req[total] = '\0';
        if( strstr( req, "\r\n\r\n" ) != NULL ) {
            break;
            }
        if( total >= (int)sizeof( req ) - 1 ) {
            break;
            }
        }

    if( !path_authorized( req ) ) {
        /* Deliberately says nothing about what a correct URL looks like. */
        static const char *body =
            "<html><head><title>cscreen</title></head>\n"
            "<body style=\"font-family:sans-serif\">\n"
            "<h2>Security code incorrect.</h2>\n"
            "</body></html>\n";
        char head[256];
        sprintf( head,
                 "HTTP/1.1 403 Forbidden\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: %lu\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n\r\n",
                 (unsigned long)strlen( body ) );
        io_send_all( sock, ssl, NULL,
                     (const unsigned char *)head, strlen( head ) );
        io_send_all( sock, ssl, NULL,
                     (const unsigned char *)body, strlen( body ) );
        }
    else if( strncmp( req, "GET /favicon.ico", 16 ) == 0 ) {
        static const char *notFound =
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        io_send_all( sock, ssl, NULL,
                     (const unsigned char *)notFound, strlen( notFound ) );
        }
    else {
        http_serve( sock, ssl );
        }

#ifdef USE_TLS
    if( ssl != NULL ) {
        SSL_shutdown( (SSL *)ssl );
        SSL_free( (SSL *)ssl );
        close( sock );
        return NULL;
        }
#endif
    shutdown( sock, SHUT_WR );
    close( sock );
    return NULL;
    }


/* ------------------------------------------------------------------ */
/* Listeners                                                           */
/* ------------------------------------------------------------------ */

static int make_listener( int port ) {
    int sock;
    int one = 1;
    struct sockaddr_in addr;

    sock = socket( AF_INET, SOCK_STREAM, 0 );
    if( sock < 0 ) {
        return -1;
        }
    setsockopt( sock, SOL_SOCKET, SO_REUSEADDR, (void *)&one, sizeof( one ) );

    memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_ANY );
    addr.sin_port = htons( (unsigned short)port );

    if( bind( sock, (struct sockaddr *)&addr, sizeof( addr ) ) != 0 ) {
        close( sock );
        return -1;
        }
    if( listen( sock, 32 ) != 0 ) {
        close( sock );
        return -1;
        }
    return sock;
    }


static void *http_accept_thread( void *arg ) {
    int listener = *(int *)arg;
    for( ;; ) {
        int *client = (int *)malloc( sizeof( int ) );
        pthread_t t;
        if( client == NULL ) {
            sleep( 1 );
            continue;
            }
        *client = accept( listener, NULL, NULL );
        if( *client < 0 ) {
            free( client );
            continue;
            }
        set_nodelay( *client );
        if( pthread_create( &t, NULL, http_conn_thread, client ) != 0 ) {
            close( *client );
            free( client );
            continue;
            }
        pthread_detach( t );
        }
    return NULL;
    }


static void relay_accept_loop( int listener ) {
    for( ;; ) {
        int client = accept( listener, NULL, NULL );
        int slot = -1;
        int i;
        Conn *c;
        pthread_t t;

        if( client < 0 ) {
            continue;
            }
        set_nodelay( client );

        pthread_mutex_lock( &gLock );
        for( i = 0; i < MAX_CONNS; i++ ) {
            if( !gConns[i].used ) {
                slot = i;
                break;
                }
            }
        if( slot < 0 ) {
            pthread_mutex_unlock( &gLock );
            close( client );
            continue;
            }
        c = &gConns[slot];
        memset( c, 0, sizeof( Conn ) );
        c->used = 1;
        c->id = gNextId++;
        c->sock = client;
        c->lastSeen = time( NULL );
        pthread_mutex_init( &c->lock, NULL );
        pthread_cond_init( &c->wake, NULL );
        pthread_mutex_init( &c->sslLock, NULL );
        pthread_mutex_unlock( &gLock );

        if( pthread_create( &t, NULL, reader_thread, c ) != 0 ) {
            close( client );
            pthread_mutex_lock( &gLock );
            c->used = 0;
            pthread_mutex_unlock( &gLock );
            continue;
            }
        pthread_detach( t );
        }
    }


#ifdef USE_TLS
/* Build the shared server SSL context once, from the operator's
   certificate chain and private key.  Returns 0 on success. */
static int tls_init( const char *certPath, const char *keyPath ) {
    gCtx = SSL_CTX_new( TLS_server_method() );
    if( gCtx == NULL ) {
        return -1;
        }
    SSL_CTX_set_min_proto_version( gCtx, TLS1_2_VERSION );
    /* No renegotiation, and tolerate the write buffer moving between a
       WANT_WRITE and its retry -- both keep the duplex relay simple. */
    SSL_CTX_set_options( gCtx, SSL_OP_NO_RENEGOTIATION );
    SSL_CTX_set_mode( gCtx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER );

    if( SSL_CTX_use_certificate_chain_file( gCtx, certPath ) != 1 ) {
        fprintf( stderr, "cannot read certificate chain from %s\n", certPath );
        return -1;
        }
    if( SSL_CTX_use_PrivateKey_file( gCtx, keyPath, SSL_FILETYPE_PEM ) != 1 ) {
        fprintf( stderr, "cannot read private key from %s\n", keyPath );
        return -1;
        }
    if( SSL_CTX_check_private_key( gCtx ) != 1 ) {
        fprintf( stderr, "private key does not match certificate\n" );
        return -1;
        }
    return 0;
    }
#endif


int main( int argc, char **argv ) {
    int httpListener;
    int relayListener;
    pthread_t t;
    const char *security = NULL;
    const char *certPath = NULL;
    const char *keyPath = NULL;
    const char *scheme = "http";

    /* Positional and unambiguous by count, because the security string is
       exactly one argument and a cert/key is exactly two:
         3 args -> ports only
         4 args -> ports + SECURITY
         5 args -> ports + CERT KEY
         6 args -> ports + SECURITY + CERT KEY                            */
    if( argc < 3 || argc > 6 ) {
        fprintf( stderr,
                 "usage: %s HTTP_PORT RELAY_PORT [SECURITY_STRING] "
                 "[FULLCHAIN_PEM PRIVKEY_PEM]\n",
                 argv[0] );
        return 1;
        }

    gHttpPort = atoi( argv[1] );
    gRelayPort = atoi( argv[2] );

    switch( argc ) {
        case 4:
            security = argv[3];
            break;
        case 5:
            certPath = argv[3];
            keyPath = argv[4];
            break;
        case 6:
            security = argv[3];
            certPath = argv[4];
            keyPath = argv[5];
            break;
        default:
            break;
        }

#ifndef USE_TLS
    /* keyPath is only consumed by the TLS build; keep the plain build's
       zero-warning bar without an #ifdef around the parsing above. */
    (void)keyPath;
#endif

    if( security != NULL && security[0] != '\0' ) {
        derive_code( security, gCode );
        }

    if( gHttpPort <= 0 || gRelayPort <= 0 || gHttpPort == gRelayPort ) {
        fprintf( stderr, "bad ports\n" );
        return 1;
        }

    if( certPath != NULL ) {
#ifdef USE_TLS
        if( tls_init( certPath, keyPath ) != 0 ) {
            return 1;
            }
        gTLS = 1;
        scheme = "https";
#else
        fprintf( stderr,
                 "a certificate was given but this binary has no TLS.\n"
                 "rebuild with:  gcc -DUSE_TLS -o cscreen -lpthread "
                 "-lssl -lcrypto cscreen.c\n" );
        return 1;
#endif
        }

    signal( SIGPIPE, SIG_IGN );
    memset( gConns, 0, sizeof( gConns ) );
    strcpy( gMime, "video/webm" );

    httpListener = make_listener( gHttpPort );
    if( httpListener < 0 ) {
        fprintf( stderr, "cannot listen on HTTP port %d\n", gHttpPort );
        return 1;
        }
    relayListener = make_listener( gRelayPort );
    if( relayListener < 0 ) {
        fprintf( stderr, "cannot listen on relay port %d\n", gRelayPort );
        return 1;
        }

    if( pthread_create( &t, NULL, http_accept_thread, &httpListener ) != 0 ) {
        fprintf( stderr, "cannot start http thread\n" );
        return 1;
        }
    pthread_detach( t );

    if( pthread_create( &t, NULL, heartbeat_thread, NULL ) != 0 ) {
        fprintf( stderr, "cannot start heartbeat thread\n" );
        return 1;
        }
    pthread_detach( t );

    printf( "cscreen ready\n" );
    if( gCode[0] != '\0' ) {
        /* Extra spaces around the URL so it is easy to double-click and  */
        /* copy out of a terminal.                                        */
        printf( "  open   %s://localhost:%d/%s   in a browser\n",
                scheme, gHttpPort, gCode );
        printf( "  (clients without that code are refused)\n" );
        }
    else {
        printf( "  open   %s://localhost:%d   in a browser\n",
                scheme, gHttpPort );
        printf( "  (no security string given: anyone who can reach this "
                "port can watch)\n" );
        }
    if( gTLS ) {
        printf( "  TLS on -- reach it by the hostname the certificate was "
                "issued for, not localhost\n" );
        }
    else {
        printf( "  no TLS: browsers only allow screen capture over http on "
                "localhost, not a remote host\n" );
        }
    printf( "  relay listening on port %d\n", gRelayPort );
    fflush( stdout );

    relay_accept_loop( relayListener );
    return 0;
    }
