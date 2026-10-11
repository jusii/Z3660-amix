#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lwip.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/tcpbase.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/xadapter.h"
#include "ff.h"

void print_hdmi_ln(int xpos, char *message, int line_inc);
#define printf2(N,...) do {char message[N]; \
    snprintf(message, N, __VA_ARGS__);\
     printf("%s\n",message); print_hdmi_ln(0,message,1); } while(0)

static struct tcp_pcb *http_pcb;
static FATFS fatfs;              /* volume 1: -- exFAT data partition (read/write) */
static int fatfs_mounted = 0;
static FATFS fatfs0;             /* volume 0: -- FAT boot partition (READ ONLY here) */
static int fatfs0_mounted = 0;

/* Mount helper from main.c, declared here so this module stays self-contained --
   same extern that sd_fileops.c uses. It is f_mount() plus a (currently
   disabled) SD clock retune, so behaviour is identical to a plain f_mount(). */
extern FRESULT f_clk_mount(FATFS* fs, const TCHAR* path, BYTE opt);

/* Bytes read from the card per refill of the per-connection send buffer. A
   multiple of the 512-byte sector keeps f_read() on FatFs' direct-transfer
   path; lwIP repacks it into MSS-sized segments. */
#define HTTPD_SEND_CHUNK 4096

struct http_conn_state {
    int close_pending;
    int upload_active; /* 1 while receiving upload body */
    FIL upload_file;   /* file being written */
    uint32_t upload_remaining; /* bytes left to receive */
    char upload_path[128];
    /* --- send engine (see http_send_pump) -------------------------------
       A response body is streamed across many tcp_sent() callbacks, so the
       open file and the not-yet-accepted bytes have to outlive the request
       handler. Uploads and downloads never overlap on one connection. */
    int send_active;            /* 1 while a body is still being streamed */
    FIL send_file;              /* open source file, at its own read offset */
    FSIZE_t send_remaining;     /* bytes of the file not yet read into send_buf */
    UINT send_buf_len;          /* valid bytes in send_buf */
    UINT send_buf_pos;          /* of those, how many tcp_write() has taken */
    BYTE send_buf[HTTPD_SEND_CHUNK];
};

#ifndef HTTPD_TRACE
#define HTTPD_TRACE() printf2(64,"[httpd] %s", __func__)
#endif

const char *mime_type(const char *name);
static err_t http_sent(void *arg, struct tcp_pcb *pcb, u16_t len);
void http_send_file(struct tcp_pcb *pcb, char *request, struct http_conn_state *state);
static void http_send_404(struct tcp_pcb *pcb, struct http_conn_state *state);
static void http_conn_close(struct tcp_pcb *pcb, struct http_conn_state *state);
static err_t send_http_header(struct tcp_pcb *pcb, const char *filename,
                              FSIZE_t size, const char *attach_name);
void extract_filename(char *request, char *filename, size_t size);
void http_send_json_system(struct tcp_pcb *pcb, struct http_conn_state *state);
void http_send_json_dirs(struct tcp_pcb *pcb, struct http_conn_state *state);
static void http_send_pump(struct tcp_pcb *pcb, struct http_conn_state *state);
static void http_send_download(struct tcp_pcb *pcb, const char *query,
                               struct http_conn_state *state);
static err_t http_poll(void *arg, struct tcp_pcb *pcb);
static void http_err(void *arg, err_t err);

/* URL-decode in-place into dst, returns dst. Simple implementation. */
static char *url_decode(char *dst, const char *src)
{
    char *o = dst;
    while (*src) {
        if (*src == '+') {
            *o++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *o++ = *src++;
        }
    }
    *o = '\0';
    return dst;
}

/* Pull one percent-decoded value out of a query string ("a=1&path=1:/x&b=2").
   Same ?path= convention the PUT /upload handler uses, parsed on '&'
   boundaries so a key is not matched inside another key's name.
   Returns 1 if the key was present. */
static int query_get(const char *query, const char *key, char *out, size_t outlen)
{
    size_t keylen = strlen(key);
    const char *p = query;

    out[0] = '\0';
    if (!query)
        return 0;

    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seglen = amp ? (size_t)(amp - p) : strlen(p);
        if (seglen > keylen && p[keylen] == '=' && !strncmp(p, key, keylen)) {
            char raw[256], decoded[256];
            size_t vlen = seglen - keylen - 1;
            if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
            memcpy(raw, p + keylen + 1, vlen);
            raw[vlen] = '\0';
            url_decode(decoded, raw);   /* never grows, so raw's size bounds it */
            size_t dlen = strlen(decoded);
            if (dlen > outlen - 1) dlen = outlen - 1;
            memcpy(out, decoded, dlen);
            out[dlen] = '\0';
            return 1;
        }
        if (!amp)
            break;
        p = amp + 1;
    }
    return 0;
}

/* Download policy for GET /download. Reads are allowed from both volumes; the
   path must name one explicitly and must not try to walk out of it.

   Note the asymmetry with PUT /upload, which is deliberate: 0: is the boot
   volume and stays read-only over HTTP. It is written only through the boot
   menu's E -> COPY path, which has FAILSAFE.bin and two CRC gates behind it. */
static int http_download_path_ok(const char *path)
{
    if (!path || !path[0])
        return 0;
    if (path[1] != ':')
        return 0;
    if (path[0] != '0' && path[0] != '1')
        return 0;
    /* Reject ".." outright rather than per segment. FatFs here is built with
       FF_FS_RPATH 0, so "." and ".." are not valid path items anyway, and a
       real file on this card never has them in its name. */
    if (strstr(path, "..") != NULL)
        return 0;
    return 1;
}

/* The single way a connection ends, and the only place the per-connection
   state is released.

   The ordering is not a style choice, it is what lwIP requires. tcp_close()
   may free the pcb before it returns -- tcp_close_shutdown() sends an RST and
   calls tcp_free() outright when the receive window is still short, i.e. when
   the request has not been tcp_recved() yet -- and when it cannot allocate the
   FIN it returns ERR_OK anyway, having flagged the pcb for a close from
   tcp_tmr. Its own comment spells out the contract: once tcp_close() has been
   called the pcb must not be used again. So there is nothing to retry on
   "failure", and anything that touches the pcb afterwards (a tcp_arg() to
   detach, say) is a use-after-free waiting for the RST path to be taken.

   Detach every callback first, so lwIP cannot hand a freed state to one of
   them; then close; then free. The caller must not touch the pcb after this
   returns. Passing a NULL state is fine -- it just closes the connection. */
static void http_conn_close(struct tcp_pcb *pcb, struct http_conn_state *state)
{
    if (state) {
        if (state->send_active) {
            f_close(&state->send_file);
            state->send_active = 0;
        }
        if (state->upload_active) {
            f_close(&state->upload_file);
            state->upload_active = 0;
        }
    }

    tcp_arg(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);

    tcp_close(pcb);
    free(state);
}

/* Hand as much of the open file to lwIP as the send window will take, then
   return. Called once from the request handler to prime the connection and
   again from every tcp_sent() callback until the body is fully queued.

   This is what makes a response bigger than TCP_SND_BUF (8 KB here) possible:
   nothing pumps lwIP while a callback runs, so no ACK can arrive and no send
   buffer can free up inside this function. The old single-pass loop wrote
   until tcp_write() said ERR_MEM and then *broke out and closed the file*, so
   every response was truncated at roughly one send-buffer's worth. Here a
   refusal only ends this pass -- the unaccepted bytes stay in send_buf, the
   file stays open at its own offset, and the next ACK resumes exactly where
   this pass stopped. */
static void http_send_pump(struct tcp_pcb *pcb, struct http_conn_state *state)
{
    int stalled = 0;

    if (!state || !state->send_active)
        return;

    while (!stalled) {
        /* 1. drain whatever is still sitting in the chunk buffer */
        while (state->send_buf_pos < state->send_buf_len) {
            u16_t space = tcp_sndbuf(pcb);
            if (space == 0) {            /* window full: wait for an ACK */
                stalled = 1;
                break;
            }
            UINT n = state->send_buf_len - state->send_buf_pos;
            if (n > space) n = space;
            if (tcp_write(pcb, state->send_buf + state->send_buf_pos, (u16_t)n,
                          TCP_WRITE_FLAG_COPY) != ERR_OK) {
                /* out of pbufs/segments right now -- keep the bytes and retry */
                stalled = 1;
                break;
            }
            state->send_buf_pos += n;
        }
        if (stalled)
            break;

        /* 2. buffer empty: either the body is fully queued, or refill it */
        if (state->send_remaining == 0)
            break;
        if (tcp_sndbuf(pcb) == 0) {
            stalled = 1;
            break;
        }

        UINT want = HTTPD_SEND_CHUNK;
        if ((FSIZE_t)want > state->send_remaining)
            want = (UINT)state->send_remaining;
        UINT br = 0;
        FRESULT fres = f_read(&state->send_file, state->send_buf, want, &br);
        if (fres != FR_OK || br == 0) {
            /* The Content-Length we already sent lets the client see the
               short body, which is the whole point of announcing it. */
            printf2(100,"[httpd] %s: f_read failed (%d, %u bytes)", __func__, (int)fres, br);
            state->send_remaining = 0;
            state->send_buf_len = 0;
            state->send_buf_pos = 0;
            break;
        }
        state->send_buf_len = br;
        state->send_buf_pos = 0;
        state->send_remaining -= br;
    }

    tcp_output(pcb);

    if (state->send_remaining == 0 && state->send_buf_pos >= state->send_buf_len) {
        f_close(&state->send_file);
        state->send_active = 0;
        state->close_pending = 1;
        printf2(64,"[httpd] %s: body fully queued, pending close", __func__);
    }
}

static err_t http_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)len;
    struct http_conn_state *state = (struct http_conn_state *)arg;
    if (!state) {
        return ERR_OK;
    }
    /* Resume a streamed body. No logging here: this fires once per ACK, and
       printf2() also paints the HDMI console -- logging per ACK would dominate
       the transfer time on a multi-megabyte download. */
    if (state->send_active)
        http_send_pump(pcb, state);

    if (!state->send_active && state->close_pending) {
        printf2(64,"[httpd] %s: response complete, closing", __func__);
        http_conn_close(pcb, state);
    }
    return ERR_OK;
}

/* Safety net for the send engine, and the backstop for every close.

   http_send_pump() normally resumes from tcp_sent(), which needs an ACK to
   fire -- and an ACK only comes if something is still unacknowledged. If a
   pass ever ends having queued nothing at all (pbufs momentarily exhausted)
   there would be no ACK left to wake it, so the coarse timer retries too.

   It is also what guarantees a connection marked close_pending is always
   reaped, whether or not another ACK ever arrives: every response path just
   sets the flag and returns, and this fires roughly every two seconds until
   the connection is gone. lwIP supports closing from here -- tcp_slowtmr()
   reads pcb->next before the callback and restarts its walk if the callback
   changed the active list. */
static err_t http_poll(void *arg, struct tcp_pcb *pcb)
{
    struct http_conn_state *state = (struct http_conn_state *)arg;
    if (!state)
        return ERR_OK;
    if (state->send_active) {
        http_send_pump(pcb, state);
        return ERR_OK;
    }
    if (state->close_pending) {
        printf2(64,"[httpd] %s: closing on poll", __func__);
        http_conn_close(pcb, state);
    }
    return ERR_OK;
}

/* The connection died under us (RST / abort). lwIP has already freed the pcb,
   so only our own state is ours to release -- including any file left open by
   an interrupted transfer. */
static void http_err(void *arg, err_t err)
{
    struct http_conn_state *state = (struct http_conn_state *)arg;
    if (!state)
        return;
    printf2(64,"[httpd] %s: connection error %d", __func__, err);
    if (state->send_active) {
        f_close(&state->send_file);
        state->send_active = 0;
    }
    if (state->upload_active) {
        f_close(&state->upload_file);
        state->upload_active = 0;
    }
    free(state);
}

err_t http_recv(void *arg,
                struct tcp_pcb *pcb,
                struct pbuf *p,
                err_t err)
{
    struct http_conn_state *state = (struct http_conn_state *)arg;
    (void)err;
//    HTTPD_TRACE();
    if (p == NULL)
    {
        /* The peer sent FIN. Closing is ours to do: freeing the state without
           closing left the pcb parked in CLOSE_WAIT for good, which leaks a
           tcp_pcb (there are MEMP_NUM_TCP_PCB of them) on every connection the
           client closed first. http_conn_close() also closes any file an
           interrupted transfer left open. */
        printf2(64,"[httpd] %s: peer closed, tearing down", __func__);
        http_conn_close(pcb, state);
        return ERR_OK;
    }

    /* Below here the state is required: it is where the response, the open
       file and the close flag live. It can only be NULL if a callback fired
       after teardown, which cannot happen because teardown detaches them all
       -- http_accept() refuses the connection outright when the allocation
       fails. Handled anyway, so no path below has to re-check it. */
    if (state == NULL)
    {
        printf2(64,"[httpd] %s: no state, dropping connection", __func__);
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        http_conn_close(pcb, NULL);
        return ERR_OK;
    }

    /* A body is still streaming out on this connection. Every response says
       "Connection: close", so a second request here is not a pipelined one to
       honour -- and acting on it would clobber the in-flight send_file. */
    if (state->send_active) {
        printf2(64,"[httpd] %s: request while sending, ignored", __func__);
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    }

    char *data = (char *)p->payload;

    /* If an upload is in progress, write the received payload to file */
    if (state->upload_active) {
        size_t to_write = p->tot_len;
        if (to_write > state->upload_remaining) to_write = state->upload_remaining;
        UINT bw;
        FRESULT fres = f_write(&state->upload_file, data, to_write, &bw);
        if (fres != FR_OK || bw != to_write) {
            printf2(64,"[httpd] %s: f_write error during upload %d", __func__, (int)fres);
            /* abort upload */
            f_close(&state->upload_file);
            state->upload_active = 0;
            state->upload_remaining = 0;
            http_send_404(pcb, state);
        } else {
            state->upload_remaining -= bw;
            // This printf spams the console, so reduce its frequency
            static int upload_log_counter = 0;
            upload_log_counter++;
            if (upload_log_counter % 100 == 0) {
            printf2(64,"[httpd] %s: wrote %u bytes, remaining %lu", __func__, bw, (unsigned long)state->upload_remaining);
            }
            if (state->upload_remaining == 0) {
                /* finish upload */
                f_close(&state->upload_file);
                state->upload_active = 0;
                /* send success response */
                const char *msg = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nUPLOAD OK";
                tcp_write(pcb, msg, strlen(msg), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb);
                state->close_pending = 1;
                printf2(1000,"[httpd] %s: upload finished %s", __func__, state->upload_path);
            }
        }
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
//        printf2(64,"[httpd] %s: return ERR_OK (upload path)", __func__);
        return ERR_OK;
    }

    if (!strncmp(data, "GET ", 4))
    {
        printf2(64,"[httpd] %s: GET request", __func__);
        http_send_file(pcb, data, state);
    }

    else if (!strncmp(data, "PUT ", 4)) {
        /* Handle simple PUT /upload/<filename> HTTP/1.1\r\n...\r\n<binary> */
        printf2(64,"[httpd] %s: PUT request", __func__);
        /* extract path */
        char filename[256];
        extract_filename(data, filename, sizeof(filename));
        /* If there is a query string, split it off */
        char *q = strchr(filename, '?');
        char query[256] = {0};
        if (q) {
            strncpy(query, q+1, sizeof(query)-1);
            *q = '\0';
        }
        /* expect /upload/<name> */
        const char *pfx = "/upload/";
        if (strncmp(filename, pfx, strlen(pfx)) != 0) {
            printf2(1000,"[httpd] %s: PUT to unsupported path %s", __func__, filename);
            http_send_404(pcb, state);
        } else {
            const char *name = filename + strlen(pfx);
            if (*name == '\0') {
                printf2(64,"[httpd] %s: PUT missing filename", __func__);
                http_send_404(pcb, state);
            } else {
                /* parse path= in query if provided */
                char dest[257] = {0};
                if (query[0]) {
                    /* look for path= value */
                    char *p = strstr(query, "path=");
                    if (p) {
                        p += 5;
                        char decoded[256];
                        url_decode(decoded, p);
                        strncpy(dest, decoded, sizeof(dest)-1);
                    }
                }
                /* default destination if none provided */
                if (dest[0] == '\0') strncpy(dest, "1:/web", sizeof(dest)-1);

                /* Validate dest: uploads go to '1:' only.
                   0: is refused explicitly. It used to be refused by accident
                   -- the destination check accepted "0:" and the f_open then
                   failed because this server had never mounted that volume.
                   GET /download does mount 0: now, so the refusal has to be
                   stated or the boot volume would silently become writable
                   over HTTP. It must not be: Z3660.bin/FAILSAFE.bin are
                   replaced only through the boot menu's E -> COPY step, which
                   is CRC-verified on both sides and keeps a failsafe image.
                   The response is a 404 either way, so nothing a client sees
                   has changed. */
                if (!(dest[0] == '1' && dest[1] == ':')) {
                    printf2(1000,"[httpd] %s: invalid destination %s (uploads are 1: only)", __func__, dest);
                    http_send_404(pcb, state);
                } else {
                    /* ensure directory exists */
                    FRESULT st = f_stat(dest, NULL);
                    if (st != FR_OK) {
                        /* try to create */
                        f_mkdir(dest);
                    }
                    char fullpath[512];
                    /* join dest and name */
                    if (dest[strlen(dest)-1] == '/')
                        snprintf(fullpath, sizeof(fullpath), "%s%s", dest, name);
                    else
                        snprintf(fullpath, sizeof(fullpath), "%s/%s", dest, name);
                    /* try to find Content-Length header */
                    char *cl = strstr(data, "Content-Length:");
                uint32_t content_len = 0;
                if (cl) {
                    cl += strlen("Content-Length:");
                    while (*cl == ' ') cl++;
                    content_len = (uint32_t)atoi(cl);
                } else {
                    printf2(64,"[httpd] %s: no Content-Length header", __func__);
                }
                /* open file for write */
                FRESULT fres = f_open(&state->upload_file, fullpath, FA_CREATE_ALWAYS | FA_WRITE);
                if (fres != FR_OK) {
                    printf2(1000,"[httpd] %s: f_open upload failed %s: %d", __func__, fullpath, (int)fres);
                    http_send_404(pcb, state);
                } else {
                    printf2(1000,"[httpd] %s: upload start %s, expecting %lu bytes", __func__, fullpath, (unsigned long)content_len);
                    state->upload_active = 1;
                    state->upload_remaining = content_len;
                    strncpy(state->upload_path, fullpath, sizeof(state->upload_path)-1);
                    state->upload_path[sizeof(state->upload_path)-1] = '\0';
                    /* find end of headers */
                    char *body = strstr(data, "\r\n\r\n");
                    if (body) {
                        body += 4;
                        size_t body_len = (char *)p->payload + p->tot_len - body;
                        if (body_len > 0) {
                            UINT bw;
                            FRESULT fres2 = f_write(&state->upload_file, body, body_len, &bw);
                            if (fres2 != FR_OK) {
                                printf2(64,"[httpd] %s: f_write initial body failed %d", __func__, (int)fres2);
                                f_close(&state->upload_file);
                                state->upload_active = 0;
                                http_send_404(pcb, state);
                            } else {
                                if (bw > state->upload_remaining) bw = state->upload_remaining;
                                state->upload_remaining -= bw;
                                printf2(64,"[httpd] %s: wrote initial %u bytes, remaining %lu", __func__, bw, (unsigned long)state->upload_remaining);
                                if (state->upload_remaining == 0) {
                                    /* finished within first packet */
                                    f_close(&state->upload_file);
                                    state->upload_active = 0;
                                    const char *msg = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nUPLOAD OK";
                                    tcp_write(pcb, msg, strlen(msg), TCP_WRITE_FLAG_COPY);
                                    tcp_output(pcb);
                                    state->close_pending = 1;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

    tcp_recved(pcb, p->tot_len);
    printf2(64,"[httpd] %s: tcp_recved %d", __func__, p->tot_len);
    pbuf_free(p);

    printf2(64,"[httpd] %s: return ERR_OK", __func__);
    return ERR_OK;
}

void extract_filename(char *request,
                      char *filename,
                      size_t size)
{
    HTTPD_TRACE();
    char *start = request + 4;

    char *end = strchr(start, ' ');

    /* A request line with no space after the target, or a target longer than
       the caller's buffer, used to run straight off the end of it (end == NULL
       made len negative and memcpy() took it as huge). Bounded both ways now. */
    size_t len = end ? (size_t)(end - start) : strnlen(start, size - 1);
    if (len > size - 1)
        len = size - 1;

    memcpy(filename, start, len);

    filename[len] = 0;

    if (!strcmp(filename, "/")) {
        printf2(64,"[httpd] %s: root request, using /index.html", __func__);
        strncpy(filename, "/index.html", size - 1);
        filename[size - 1] = 0;
    }
}
char ip_address[30]={0};
char firmware[300]={0};
void http_send_json_system(struct tcp_pcb *pcb, struct http_conn_state *state)
{
    HTTPD_TRACE();
    /* Every exit from here marks the connection for close, including the
       failure ones. They used to just return: nothing then set close_pending,
       so neither http_sent() nor http_poll() would ever tear the connection
       down, and both the state and the pcb were stranded until the client
       gave up. Same lifetime rule as the 404 path. */
    if (state)
        state->close_pending = 1;

    DWORD free_clusters;
    FATFS *fs = &fatfs;
    FRESULT fres = f_getfree("1:", &free_clusters, &fs);
    
    unsigned long free_bytes = 0;
    if (fres == FR_OK) {
        free_bytes = free_clusters * fs->csize * 512;  /* csize is cluster size in sectors */
    }
    
    char json_buf[512];
    int json_len = snprintf(json_buf, sizeof(json_buf),
        "{"
        "\"firmware\":\"%s\","
        "\"ip\":\"%s\","
        "\"free_mb\":%lu,"
        "\"sd_mounted\":1"
        "}", firmware, ip_address,
        free_bytes / (1024 * 1024));
    
    if (json_len < 0 || json_len >= (int)sizeof(json_buf)) {
        printf2(64,"[httpd] %s: json snprintf failed", __func__);
        return;
    }
    
    /* Enviar cabecera HTTP con Content-Type JSON */
    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n");
    
    err_t err = tcp_write(pcb, header, header_len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf2(64,"[httpd] %s: header write failed", __func__);
        return;
    }
    
    /* Enviar JSON */
    err = tcp_write(pcb, json_buf, json_len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf2(64,"[httpd] %s: json write failed", __func__);
        return;
    }
    
    tcp_output(pcb);
    printf2(100,"[httpd] %s: sent json system info (%d bytes)", __func__, json_len);
    printf2(1000,"jsonn %s",json_buf);
}

void http_send_json_dirs(struct tcp_pcb *pcb, struct http_conn_state *state)
{
    HTTPD_TRACE();
    /* Candidate paths we expose to the user. The shipped web UI feeds this
       list straight into the upload destination selector, so it must only
       name places an upload can actually land -- i.e. volume 1:. "0:" was in
       the list upstream but never reached the client, because f_stat() failed
       on an unmounted volume; now that GET /download mounts 0: it would start
       showing up as an upload target that always 404s. Dropped, so the UI
       behaves exactly as before with no change to its JavaScript. */
    const char *candidates[] = { "1:/web",
                                 "1:/web/locales",
                                 "1:/hdf",
                                 "1:/kicks",
                                 "1:/presets",
                                 "1:" };
    const int ncand = sizeof(candidates)/sizeof(candidates[0]);

    char json[512];
    int off = 0;
    off += snprintf(json+off, sizeof(json)-off, "{\"dirs\":[");
    int first = 1;
    for (int i = 0; i < ncand; ++i) {
        const char *p = candidates[i];
        FILINFO fi;
        FRESULT res = f_stat(p, &fi);
        /* For root paths (0:/, 1:/) always include them even if f_stat fails */
        int is_root = (strcmp(p, "0:/") == 0 || strcmp(p, "1:/") == 0);
        if (res == FR_OK || is_root) {
            if (!first) off += snprintf(json+off, sizeof(json)-off, ",");
            off += snprintf(json+off, sizeof(json)-off, "\"%s\"", p);
            first = 0;
        } else {
            /* try to create uploads dir if candidate ends with /uploads */
            if (strstr(p, "/uploads") != NULL) {
                FRESULT mres = f_mkdir(p);
                if (mres == FR_OK) {
                    if (!first) off += snprintf(json+off, sizeof(json)-off, ",");
                    off += snprintf(json+off, sizeof(json)-off, "\"%s\"", p);
                    first = 0;
                }
            }
        }
    }
    off += snprintf(json+off, sizeof(json)-off, "]}\n");
    printf("json dirs:\n%s\n", json);

    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n");
    tcp_write(pcb, header, header_len, TCP_WRITE_FLAG_COPY);
    tcp_write(pcb, json, off, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    if (state) state->close_pending = 1;
}
/*
const char *mime_type(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "text/html";
    if (!strcmp(dot, ".css")) return "text/css";
    if (!strcmp(dot, ".js")) return "application/javascript";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcmp(dot, ".gif")) return "image/gif";
    if (!strcmp(dot, ".svg")) return "image/svg+xml";
    return "application/octet-stream";
}
*/
void http_send_file(struct tcp_pcb *pcb, char *request, struct http_conn_state *state)
{
    HTTPD_TRACE();
    /* Either volume is enough to serve something: the web assets need 1:, but
       a download off 0: should still work if 1: failed to mount. Whichever
       volume a request actually needs is re-checked where it is opened. */
    if (!fatfs_mounted && !fatfs0_mounted) {
        printf2(64,"[httpd] %s: fatfs not mounted", __func__);
        http_send_404(pcb, state);
        return;
    }

    char filename[256];
    extract_filename(request, filename, sizeof(filename));

    /* Split off the query string before routing, so "/download?path=..." (and
       an /api endpoint carrying a cache-buster) match on the path alone. */
    char query[256];
    char *q = strchr(filename, '?');
    query[0] = '\0';
    if (q) {
        strncpy(query, q + 1, sizeof(query) - 1);
        query[sizeof(query) - 1] = '\0';
        *q = '\0';
    }

    /* Check for API endpoints */
    if (!strcmp(filename, "/api/system")) {
        printf2(64,"[httpd] %s: api/system request", __func__);
        http_send_json_system(pcb, state);
        return;
    }
    if (!strcmp(filename, "/api/dirs")) {
        printf2(64,"[httpd] %s: api/dirs request", __func__);
        http_send_json_dirs(pcb, state);
        return;
    }
    if (!strcmp(filename, "/download")) {
        http_send_download(pcb, q ? query : NULL, state);
        return;
    }

    if (!state) {
        printf2(64,"[httpd] %s: no connection state", __func__);
        http_send_404(pcb, state);
        return;
    }

    char fullpath[sizeof(filename) + 8];   /* "1:/web" + the longest filename */
    snprintf(fullpath, sizeof(fullpath), "1:/web%s", filename);
    FRESULT fres = f_open(&state->send_file, fullpath, FA_READ);
    if (fres != FR_OK)
    {
        printf2(1000,"[httpd] %s: f_open failed for %s: %d", __func__, fullpath, (int)fres);
        http_send_404(pcb, state);
        printf2(64,"[httpd] %s: returning after 404", __func__);
        return;
    }
    else {
        printf2(1000,"[httpd] %s: f_open succeeded for %s", __func__, fullpath);
    }

    /* Same drain-aware send engine as /download: a web asset larger than the
       send buffer used to be truncated here too. */
    FSIZE_t size = f_size(&state->send_file);
    if (send_http_header(pcb, fullpath, size, NULL) != ERR_OK) {
        f_close(&state->send_file);
        http_send_404(pcb, state);
        return;
    }
    state->send_active = 1;
    state->send_remaining = size;
    state->send_buf_len = 0;
    state->send_buf_pos = 0;
    http_send_pump(pcb, state);
}

/* GET /download?path=<vol>:/<file> -- read any file on 0: or 1: back to the
   host. Same ?path= query convention as PUT /upload/<name>?path=..., but this
   one accepts both volumes because reading 0: is harmless; only writing it is
   gated (see http_download_path_ok). */
static void http_send_download(struct tcp_pcb *pcb, const char *query,
                               struct http_conn_state *state)
{
    HTTPD_TRACE();
    char path[256];

    if (!state) {
        printf2(64,"[httpd] %s: no connection state", __func__);
        http_send_404(pcb, state);
        return;
    }
    if (!query_get(query, "path", path, sizeof(path)) || path[0] == '\0') {
        printf2(64,"[httpd] %s: no path= in query", __func__);
        http_send_404(pcb, state);
        return;
    }
    if (!http_download_path_ok(path)) {
        printf2(1000,"[httpd] %s: rejected path %s", __func__, path);
        http_send_404(pcb, state);
        return;
    }
    if ((path[0] == '0' && !fatfs0_mounted) || (path[0] == '1' && !fatfs_mounted)) {
        printf2(1000,"[httpd] %s: volume not mounted for %s", __func__, path);
        http_send_404(pcb, state);
        return;
    }

    FRESULT fres = f_open(&state->send_file, path, FA_READ);
    if (fres != FR_OK) {
        printf2(1000,"[httpd] %s: f_open failed for %s: %d", __func__, path, (int)fres);
        http_send_404(pcb, state);
        return;
    }

    FSIZE_t size = f_size(&state->send_file);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    if (send_http_header(pcb, path, size, base) != ERR_OK) {
        f_close(&state->send_file);
        http_send_404(pcb, state);
        return;
    }
    state->send_active = 1;
    state->send_remaining = size;
    state->send_buf_len = 0;
    state->send_buf_pos = 0;
    printf2(1000,"[httpd] %s: download start %s (%llu bytes)", __func__, path,
            (unsigned long long)size);
    http_send_pump(pcb, state);
}

/* 200 header for a file body. Content-Length is what lets the host tell a
   complete transfer from a truncated one, so it is always sent; attach_name,
   when given, additionally offers the file as a download in a browser. */
static err_t send_http_header(struct tcp_pcb *pcb, const char *filename,
                              FSIZE_t size, const char *attach_name)
{
    HTTPD_TRACE();
    const char *type = mime_type(filename);
    char header[512];
    int len;

    if (attach_name)
        len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %llu\r\n"
            "Content-Disposition: attachment; filename=\"%s\"\r\n"
            "Connection: close\r\n"
            "\r\n",
            type, (unsigned long long)size, attach_name);
    else
        len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %llu\r\n"
            "Connection: close\r\n"
            "\r\n",
            type, (unsigned long long)size);

    if (len < 0 || len >= (int)sizeof(header)) {
        printf2(64,"[httpd] %s: header snprintf failed", __func__);
        return ERR_VAL;
    }

    err_t header_err = tcp_write(pcb,
              header,
              len,
              TCP_WRITE_FLAG_COPY);
    if (header_err != ERR_OK) {
        printf2(64,"[httpd] %s: tcp_write(header) failed %d", __func__, header_err);
    } else {
        printf2(64,"[httpd] %s: tcp_write(header) ok %d", __func__, len);
    }
    return header_err;
}

/* Queue a 404 and mark the connection for close, exactly like every other
   response path. It deliberately does NOT close here.

   Closing inline was wrong twice over. It leaked the per-connection state --
   nothing freed it, and after tcp_close() no callback of ours runs to do it
   later. And it closed while the request pbuf was still unacknowledged, which
   is the case tcp_close_shutdown() answers with an RST plus an immediate
   tcp_free() of the pcb: the client lost the 404 body it was meant to read,
   and the caller went on to use a freed pcb. Setting the flag instead lets
   http_recv() finish the request (tcp_recved() included) and leaves the close
   to http_sent()/http_poll(), which get a graceful FIN and a freed state. */
static void http_send_404(struct tcp_pcb *pcb, struct http_conn_state *state)
{
    HTTPD_TRACE();
    static const char msg[] =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html><body><h1>404</h1></body></html>";

    tcp_write(pcb,
              msg,
              strlen(msg),
              TCP_WRITE_FLAG_COPY);

    tcp_output(pcb);

    if (state) {
        state->close_pending = 1;
    } else {
        /* Unreachable: http_recv() drops a stateless connection before it can
           reach any handler. Left deliberately without a close so that no
           caller can be handed a pcb that died under it. */
        printf2(64,"[httpd] %s: 404 with no state", __func__);
    }
}

const char *mime_type(const char *name)
{
    HTTPD_TRACE();
    const char *ext = strrchr(name, '.');

    if (!ext) {
        printf2(64,"[httpd] %s: no extension for %s", __func__, name);
        return "application/octet-stream";
    }

    if (!strcmp(ext, ".html")) {
        printf2(64,"[httpd] %s: .html -> text/html", __func__);
        return "text/html";
    }

    if (!strcmp(ext, ".css")) {
        printf2(64,"[httpd] %s: .css -> text/css", __func__);
        return "text/css";
    }

    if (!strcmp(ext, ".js")) {
        printf2(64,"[httpd] %s: .js -> application/javascript", __func__);
        return "application/javascript";
    }

    /* the shipped UI fetches the .json files under web/locales; without this
       they go out as octet-stream and a strict client refuses to parse them */
    if (!strcmp(ext, ".json")) {
        printf2(64,"[httpd] %s: .json -> application/json", __func__);
        return "application/json";
    }

    if (!strcmp(ext, ".png")) {
        printf2(64,"[httpd] %s: .png -> image/png", __func__);
        return "image/png";
    }

    if (!strcmp(ext, ".jpg")) {
        printf2(64,"[httpd] %s: .jpg -> image/jpeg", __func__);
        return "image/jpeg";
    }

    if (!strcmp(ext, ".gif")) {
        printf2(64,"[httpd] %s: .gif -> image/gif", __func__);
        return "image/gif";
    }

    if (!strcmp(ext, ".svg")) {
        printf2(64,"[httpd] %s: .svg -> image/svg+xml", __func__);
        return "image/svg+xml";
    }

    printf2(64,"[httpd] %s: unknown ext %s -> octet-stream", __func__, ext);
    return "application/octet-stream";
}


err_t http_accept(void *arg,
                  struct tcp_pcb *newpcb,
                  err_t err)
{
    (void)arg;
    (void)err;
    HTTPD_TRACE();
    printf2(64,"[httpd] %s: new pcb %p", __func__, (void *)newpcb);
    struct http_conn_state *state = malloc(sizeof(*state));
    if (!state) {
        /* Returning anything but ERR_OK makes lwIP tcp_abort() this pcb for
           us (tcp_process(), the "accept function returns with an error"
           branch). Closing it here as well handed that abort an already-freed
           pcb, so the refusal is left entirely to lwIP. */
        printf2(64,"[httpd] %s: malloc failed, refusing connection", __func__);
        return ERR_MEM;
    }
    memset(state, 0, sizeof(*state));
    state->close_pending = 0;
    tcp_arg(newpcb, state);
    tcp_sent(newpcb, http_sent);
    tcp_recv(newpcb, http_recv);
    tcp_err(newpcb, http_err);
    tcp_poll(newpcb, http_poll, 4);   /* 4 coarse ticks == ~2 s */

    printf2(64,"[httpd] %s: return ERR_OK", __func__);
    return ERR_OK;
}

void http_server_init(void)
{
    HTTPD_TRACE();

    if (!fatfs_mounted) {
        FRESULT res = f_mount(&fatfs, "1:", 1);
        if (res != FR_OK) {
            printf2(64,"[httpd] f_mount failed: %d", (int)res);
        } else {
            fatfs_mounted = 1;
            printf2(64, "[httpd] FATFS mounted on 1:");
        }
    }

    /* Volume 0: is mounted too, but only so GET /download can read it (the
       firmware images and the boot config live there). Nothing in this server
       opens 0: for writing -- see the note on http_download_path_ok. Same
       both-volumes mount the SD file manager does in sd_fileops.c. */
    if (!fatfs0_mounted) {
        FRESULT res = f_clk_mount(&fatfs0, "0:", 1);
        if (res != FR_OK) {
            printf2(64,"[httpd] f_mount 0: failed: %d", (int)res);
        } else {
            fatfs0_mounted = 1;
            printf2(64, "[httpd] FATFS mounted on 0: (read only)");
        }
    }

    http_pcb = tcp_new();

    printf2(64, "[httpd] %s: binding to port 80", __func__);
    tcp_bind(http_pcb, IP_ADDR_ANY, 80);

    http_pcb = tcp_listen(http_pcb);

    tcp_accept(http_pcb, http_accept);
}

extern struct netif server_netif;
void start_httpd(void)
{
    HTTPD_TRACE();
    http_server_init();
}
void run_httpd(void)
{
//    HTTPD_TRACE();
    xemacif_input(&server_netif);
    sys_check_timeouts();
}