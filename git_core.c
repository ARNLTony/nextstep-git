/*
 * git_core.c - Git client core for NeXTSTEP 3.3
 *
 * Pure logic layer: no printf, no stdin, no GUI calls.
 * Returns data via structs; reports progress via callback.
 *
 * Build: cc -O -c git_core.c
 *
 * (c) 2026 ARNLTony & Claude. MIT License.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/dir.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* Crypto Ancienne TLS library */
#include "cryanc.c"

/* Our header */
#include "git_core.h"

/* --- NeXTSTEP compatibility --- */

/* NeXTSTEP 3.3 lacks S_ISDIR/S_ISREG macros */
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

/* --- Configuration --- */

#define API_HOST      "api.github.com"
#define API_PORT      443

/* --- TLS helpers (from gh.c) --- */

int https_send_pending(sockfd, context)
int sockfd;
struct TLSContext *context;
{
    unsigned int out_buffer_len = 0;
    unsigned int out_buffer_index = 0;
    int send_res = 0;
    const unsigned char *out_buffer;

    out_buffer = tls_get_write_buffer(context, &out_buffer_len);
    while (out_buffer && out_buffer_len > 0) {
        int res = send(sockfd, (char *)&out_buffer[out_buffer_index],
                       out_buffer_len, 0);
        if (res <= 0) {
            send_res = res;
            break;
        }
        out_buffer_len -= res;
        out_buffer_index += res;
    }
    tls_buffer_clear(context);
    return send_res;
}

int validate_certificate(context, certificate_chain, len)
struct TLSContext *context;
struct TLSCertificate **certificate_chain;
int len;
{
    return no_error;
}

/* --- JSON helpers (from gh.c) --- */

char *json_find_string(json, key, out_len)
char *json;
char *key;
int *out_len;
{
    char pattern[256];
    char *p, *start, *search;

    sprintf(pattern, "\"%s\"", key);
    search = json;

    while (1) {
        p = strstr(search, pattern);
        if (!p) return NULL;

        p += strlen(pattern);

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;

        if (*p != ':') {
            search = p;
            continue;
        }
        p++;

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;

        if (*p != '"') {
            search = p;
            continue;
        }
        p++;
        start = p;

        while (*p && !(*p == '"' && *(p-1) != '\\'))
            p++;

        *out_len = p - start;
        return start;
    }
}

long json_find_number(json, key)
char *json;
char *key;
{
    char pattern[256];
    char *p, *search;
    long val;

    sprintf(pattern, "\"%s\"", key);
    search = json;

    while (1) {
        p = strstr(search, pattern);
        if (!p) return -1;

        p += strlen(pattern);

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;

        if (*p != ':') {
            search = p;
            continue;
        }
        p++;

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;

        if (*p == '-' || (*p >= '0' && *p <= '9')) {
            val = atol(p);
            return val;
        }

        search = p;
    }
}

int json_find_bool(json, key)
char *json;
char *key;
{
    char pattern[256];
    char *p, *search;

    sprintf(pattern, "\"%s\"", key);
    search = json;

    while (1) {
        p = strstr(search, pattern);
        if (!p) return -1;

        p += strlen(pattern);

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;

        if (*p != ':') {
            search = p;
            continue;
        }
        p++;

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;

        if (strncmp(p, "true", 4) == 0) return 1;
        if (strncmp(p, "false", 5) == 0) return 0;

        search = p;
    }
}

char *json_array_first(json, end)
char *json;
char **end;
{
    char *p;
    int depth;

    p = json;
    while (*p && *p != '[') p++;
    if (!*p) return NULL;
    p++;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;

    if (*p == ']') return NULL;
    if (*p != '{') return NULL;

    depth = 0;
    *end = p;
    while (**end) {
        if (**end == '{') depth++;
        else if (**end == '}') {
            depth--;
            if (depth == 0) {
                (*end)++;
                return p;
            }
        } else if (**end == '"') {
            (*end)++;
            while (**end && !(**end == '"' && *(*end - 1) != '\\'))
                (*end)++;
        }
        (*end)++;
    }
    return NULL;
}

char *json_array_next(pos, end)
char *pos;
char **end;
{
    char *p;
    int depth;

    p = *end;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;

    if (*p != ',') return NULL;
    p++;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;

    if (*p != '{') return NULL;

    depth = 0;
    *end = p;
    while (**end) {
        if (**end == '{') depth++;
        else if (**end == '}') {
            depth--;
            if (depth == 0) {
                (*end)++;
                return p;
            }
        } else if (**end == '"') {
            (*end)++;
            while (**end && !(**end == '"' && *(*end - 1) != '\\'))
                (*end)++;
        }
        (*end)++;
    }
    return NULL;
}

void json_unescape(src, src_len, dst, dst_size)
char *src;
int src_len;
char *dst;
int dst_size;
{
    int i, j;

    j = 0;
    for (i = 0; i < src_len && j < dst_size - 1; i++) {
        if (src[i] == '\\' && i + 1 < src_len) {
            i++;
            switch (src[i]) {
                case 'n':  dst[j++] = '\n'; break;
                case 't':  dst[j++] = '\t'; break;
                case '"':  dst[j++] = '"';  break;
                case '\\': dst[j++] = '\\'; break;
                case '/':  dst[j++] = '/';  break;
                default:   dst[j++] = src[i]; break;
            }
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

void json_escape(src, dst, dst_size)
char *src;
char *dst;
int dst_size;
{
    int i, j;

    j = 0;
    for (i = 0; src[i] && j < dst_size - 2; i++) {
        switch (src[i]) {
            case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
            case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
            case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
            case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
            case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
            default:
                if ((unsigned char)src[i] < 0x20) {
                    /* skip other control characters */
                } else {
                    dst[j++] = src[i];
                }
                break;
        }
    }
    dst[j] = '\0';
}

/* --- Base64 decoder (from gh.c, renamed) --- */

static char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(c)
int c;
{
    char *p;
    if (c == '=') return 0;
    p = strchr(b64_table, c);
    if (!p) return -1;
    return p - b64_table;
}

int gh_base64_decode(src, src_len, dst, dst_size)
char *src;
int src_len;
char *dst;
int dst_size;
{
    int i, j, a, b, c, d;

    j = 0;
    i = 0;
    while (i < src_len && j < dst_size - 1) {
        /* skip whitespace and newlines */
        while (i < src_len && (src[i] == '\n' || src[i] == '\r' ||
               src[i] == ' ' || src[i] == '\t' || src[i] == '\\'))
        {
            if (src[i] == '\\' && i + 1 < src_len && src[i+1] == 'n') {
                i += 2;
            } else {
                i++;
            }
        }
        if (i + 3 >= src_len) break;

        a = b64_val(src[i]);
        b = b64_val(src[i+1]);
        c = b64_val(src[i+2]);
        d = b64_val(src[i+3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) { i += 4; continue; }

        if (j < dst_size - 1) dst[j++] = (a << 2) | (b >> 4);
        if (src[i+2] != '=' && j < dst_size - 1) dst[j++] = ((b & 0x0F) << 4) | (c >> 2);
        if (src[i+3] != '=' && j < dst_size - 1) dst[j++] = ((c & 0x03) << 6) | d;
        i += 4;
    }
    dst[j] = '\0';
    return j;
}

/* --- Base64 encoder (NEW, for push) --- */

int gh_base64_encode(src, src_len, dst, dst_size)
char *src;
int src_len;
char *dst;
int dst_size;
{
    int i, j;
    unsigned char a, b, c;

    j = 0;
    for (i = 0; i < src_len && j < dst_size - 4; i += 3) {
        a = (unsigned char)src[i];
        b = (i + 1 < src_len) ? (unsigned char)src[i+1] : 0;
        c = (i + 2 < src_len) ? (unsigned char)src[i+2] : 0;

        dst[j++] = b64_table[(a >> 2) & 0x3F];
        dst[j++] = b64_table[((a & 0x03) << 4) | ((b >> 4) & 0x0F)];

        if (i + 1 < src_len) {
            dst[j++] = b64_table[((b & 0x0F) << 2) | ((c >> 6) & 0x03)];
        } else {
            dst[j++] = '=';
        }

        if (i + 2 < src_len) {
            dst[j++] = b64_table[c & 0x3F];
        } else {
            dst[j++] = '=';
        }
    }
    dst[j] = '\0';
    return j;
}

/* --- GitHub API request engine --- */

/*
 * Make a GitHub API request. Returns HTTP status code (e.g. 200, 404),
 * or a negative GIT_ERR_* code on connection failure.
 * No printf/fprintf — pure logic.
 */
int gh_api_request(token, method, path, post_body, response, response_size)
char *token;
char *method;
char *path;
char *post_body;
char *response;
int response_size;
{
    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    struct TLSContext *context;
    char *http_request;
    int req_len, body_len;
    char *resp_data;
    int resp_len;
    int read_size;
    int sent;
    char *body_start;
    char *status_line;
    int status_code;
    unsigned char tls_buf[GIT_HTTP_BUF];

    response[0] = '\0';
    body_len = post_body ? strlen(post_body) : 0;

    /* Build HTTP request */
    http_request = (char *)malloc(body_len + 2048);
    if (!http_request) {
        strcpy(response, "{\"message\":\"Out of memory\"}");
        return GIT_ERR_MEMORY;
    }

    if (post_body) {
        sprintf(http_request,
            "%s %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "Authorization: Bearer %s\r\n"
            "User-Agent: nextstep-git/1.0\r\n"
            "Accept: application/vnd.github+json\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            method, path, API_HOST, token, body_len, post_body);
    } else {
        sprintf(http_request,
            "%s %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "Authorization: Bearer %s\r\n"
            "User-Agent: nextstep-git/1.0\r\n"
            "Accept: application/vnd.github+json\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, path, API_HOST, token);
    }
    req_len = strlen(http_request);

    /* Resolve hostname */
    server = gethostbyname(API_HOST);
    if (!server) {
        free(http_request);
        strcpy(response, "{\"message\":\"DNS lookup failed\"}");
        return GIT_ERR_NETWORK;
    }

    /* Create socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        free(http_request);
        strcpy(response, "{\"message\":\"Socket creation failed\"}");
        return GIT_ERR_NETWORK;
    }

    memset((char *)&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy((char *)&serv_addr.sin_addr.s_addr,
           (char *)server->h_addr, server->h_length);
    serv_addr.sin_port = htons(API_PORT);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        free(http_request);
        close(sockfd);
        strcpy(response, "{\"message\":\"Connection failed\"}");
        return GIT_ERR_NETWORK;
    }

    /* TLS handshake */
    context = tls_create_context(0, TLS_V12);
    if (!context || !tls_sni_set(context, API_HOST)) {
        free(http_request);
        close(sockfd);
        strcpy(response, "{\"message\":\"TLS setup failed\"}");
        return GIT_ERR_NETWORK;
    }
    tls_client_connect(context);
    https_send_pending(sockfd, context);

    /* Complete handshake and send request */
    sent = 0;
    resp_data = (char *)malloc(response_size);
    if (!resp_data) {
        free(http_request);
        tls_destroy_context(context);
        close(sockfd);
        strcpy(response, "{\"message\":\"Out of memory\"}");
        return GIT_ERR_MEMORY;
    }
    resp_len = 0;

    while (1) {
        read_size = recv(sockfd, (char *)tls_buf, sizeof(tls_buf), 0);
        if (read_size <= 0) break;

        tls_consume_stream(context, tls_buf, read_size,
                           validate_certificate);
        https_send_pending(sockfd, context);

        if (!tls_established(context))
            continue;

        if (!sent) {
            /* Send in chunks to respect TLS record size limits */
            int offset = 0;
            int chunk;
            while (offset < req_len) {
                chunk = req_len - offset;
                if (chunk > 8192) chunk = 8192;
                tls_write(context, (unsigned char *)http_request + offset, chunk);
                https_send_pending(sockfd, context);
                offset += chunk;
            }
            sent = 1;
        }

        while ((read_size = tls_read(context, tls_buf, sizeof(tls_buf) - 1)) > 0) {
            if (resp_len + read_size < response_size - 1) {
                memcpy(resp_data + resp_len, tls_buf, read_size);
                resp_len += read_size;
            }
        }
    }

    resp_data[resp_len] = '\0';
    free(http_request);
    tls_destroy_context(context);
    close(sockfd);

    /* Extract HTTP status code */
    status_code = 0;
    status_line = strstr(resp_data, "HTTP/");
    if (status_line) {
        char *sp = strchr(status_line, ' ');
        if (sp) status_code = atoi(sp + 1);
    }

    /* Skip HTTP headers, copy body to response */
    body_start = strstr(resp_data, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        strncpy(response, body_start, response_size - 1);
        response[response_size - 1] = '\0';
    } else {
        strncpy(response, resp_data, response_size - 1);
        response[response_size - 1] = '\0';
    }

    free(resp_data);
    return status_code;
}

/* --- File utilities --- */

/*
 * Read entire file into malloc'd buffer. Returns NULL on error.
 * Sets *size_out to file size if non-NULL.
 */
char *git_read_file(filepath, size_out)
char *filepath;
long *size_out;
{
    int fd;
    struct stat st;
    char *buf;
    long n, total;

    if (size_out) *size_out = 0;

    if (stat(filepath, &st) < 0) return NULL;
    if (!S_ISREG(st.st_mode)) return NULL;

    fd = open(filepath, O_RDONLY);
    if (fd < 0) return NULL;

    buf = (char *)malloc(st.st_size + 1);
    if (!buf) {
        close(fd);
        return NULL;
    }

    total = 0;
    while (total < st.st_size) {
        n = read(fd, buf + total, st.st_size - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);

    buf[total] = '\0';
    if (size_out) *size_out = total;
    return buf;
}

/*
 * Write data to file. Creates/overwrites. Returns 0 on success, -1 on error.
 */
int git_write_file(filepath, data, size)
char *filepath;
char *data;
long size;
{
    int fd;
    long n, total;

    fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    total = 0;
    while (total < size) {
        n = write(fd, data + total, size - total);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        total += n;
    }
    close(fd);
    return 0;
}

int git_is_directory(path)
char *path;
{
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int git_is_file(path)
char *path;
{
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

/*
 * Create directory and all parents (like mkdir -p).
 * Returns 0 on success, -1 on error.
 */
int git_mkdir_p(path)
char *path;
{
    char tmp[GIT_MAX_PATH];
    char *p;
    int len;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    len = strlen(tmp);

    /* remove trailing slash */
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!git_is_directory(tmp)) {
                if (mkdir(tmp, 0755) < 0) return -1;
            }
            *p = '/';
        }
    }

    if (!git_is_directory(tmp)) {
        if (mkdir(tmp, 0755) < 0) return -1;
    }

    return 0;
}

/*
 * Recursively scan directory for files.
 * Skips entries starting with '.' (hidden files/dirs).
 * Paths are stored relative to the initial dirpath.
 * Returns number of files found.
 */
static int scan_dir_recursive(basepath, relpath, paths, max_entries, count)
char *basepath;
char *relpath;
char paths[][GIT_MAX_PATH];
int max_entries;
int count;
{
    DIR *dir;
    struct direct *entry;
    char fullpath[GIT_MAX_PATH];
    char newrel[GIT_MAX_PATH];
    struct stat st;

    if (relpath[0]) {
        sprintf(fullpath, "%s/%s", basepath, relpath);
    } else {
        strcpy(fullpath, basepath);
    }

    dir = opendir(fullpath);
    if (!dir) return count;

    while ((entry = readdir(dir)) != NULL) {
        if (count >= max_entries) break;

        /* skip . and .. and hidden files */
        if (entry->d_name[0] == '.') continue;

        /* build full path for stat */
        if (relpath[0]) {
            sprintf(newrel, "%s/%s", relpath, entry->d_name);
        } else {
            strcpy(newrel, entry->d_name);
        }

        sprintf(fullpath, "%s/%s", basepath, newrel);

        if (stat(fullpath, &st) < 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* recurse into subdirectory */
            count = scan_dir_recursive(basepath, newrel, paths,
                                       max_entries, count);
        } else if (S_ISREG(st.st_mode)) {
            strncpy(paths[count], newrel, GIT_MAX_PATH - 1);
            paths[count][GIT_MAX_PATH - 1] = '\0';
            count++;
        }
    }

    closedir(dir);
    return count;
}

int git_scan_directory(dirpath, paths, max_entries)
char *dirpath;
char paths[][GIT_MAX_PATH];
int max_entries;
{
    return scan_dir_recursive(dirpath, "", paths, max_entries, 0);
}

/* --- Simple SHA1-like hash for file content comparison --- */
/* We use CRC32 as a quick content hash since real SHA1 is heavy */

static unsigned long crc32_table[256];
static int crc32_inited = 0;

static void crc32_init()
{
    unsigned long c;
    int n, k;

    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++) {
            if (c & 1)
                c = 0xEDB88320L ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc32_table[n] = c;
    }
    crc32_inited = 1;
}

static unsigned long crc32_calc(data, len)
char *data;
long len;
{
    unsigned long crc;
    long i;

    if (!crc32_inited) crc32_init();

    crc = 0xFFFFFFFFL;
    for (i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ (unsigned char)data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFL;
}

/* Format CRC32 as 8-char hex, pad to look sha-ish */
static void content_hash(data, len, out)
char *data;
long len;
char *out;
{
    unsigned long crc;
    char hex[] = "0123456789abcdef";
    int i;

    crc = crc32_calc(data, len);

    /* produce 8-char hex from CRC32, pad with zeros to fill GIT_MAX_SHA-1 chars */
    for (i = 0; i < 8; i++) {
        out[7 - i] = hex[crc & 0x0F];
        crc >>= 4;
    }
    /* pad remainder with zeros to make it look like a SHA */
    for (i = 8; i < 40; i++) {
        out[i] = '0';
    }
    out[40] = '\0';
}

/* --- State persistence --- */

/*
 * State file format (.nextstep_git):
 *   Line 1: owner
 *   Line 2: repo
 *   Line 3: branch
 *   Line 4: head_sha
 *   Line 5: tree_sha
 *   Line 6: file_count
 *   Lines 7+: path\tsha\tstatus\tsize  (one per file)
 */

GitResult git_load_state(r, out)
GitRepo *r;
GitFileList *out;
{
    GitResult result;
    char statepath[GIT_MAX_PATH];
    char *data;
    long fsize;
    char *line, *next;
    int line_num, i;
    int file_count;
    char *tab1, *tab2, *tab3;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    out->count = 0;

    sprintf(statepath, "%s/%s", r->local_path, GIT_STATE_FILE);
    data = git_read_file(statepath, &fsize);
    if (!data) {
        result.code = GIT_ERR_STATE;
        sprintf(result.message, "No state file found (not a cloned repo?)");
        return result;
    }

    line_num = 0;
    file_count = 0;
    line = data;

    while (line && *line) {
        /* find end of line */
        next = strchr(line, '\n');
        if (next) {
            *next = '\0';
            next++;
        }

        /* strip CR */
        {
            int ll = strlen(line);
            if (ll > 0 && line[ll-1] == '\r')
                line[ll-1] = '\0';
        }

        line_num++;

        if (line_num == 1) {
            strncpy(r->owner, line, GIT_MAX_OWNER - 1);
        } else if (line_num == 2) {
            strncpy(r->repo, line, GIT_MAX_REPO - 1);
        } else if (line_num == 3) {
            strncpy(r->branch, line, GIT_MAX_BRANCH - 1);
        } else if (line_num == 4) {
            strncpy(r->head_sha, line, GIT_MAX_SHA - 1);
        } else if (line_num == 5) {
            strncpy(r->tree_sha, line, GIT_MAX_SHA - 1);
        } else if (line_num == 6) {
            file_count = atoi(line);
        } else {
            /* parse file entry: path\tsha\tstatus\tsize */
            i = out->count;
            if (i >= GIT_MAX_FILES) break;

            tab1 = strchr(line, '\t');
            if (!tab1) goto next_line;
            *tab1 = '\0';
            tab1++;

            tab2 = strchr(tab1, '\t');
            if (!tab2) goto next_line;
            *tab2 = '\0';
            tab2++;

            tab3 = strchr(tab2, '\t');
            if (!tab3) goto next_line;
            *tab3 = '\0';
            tab3++;

            strncpy(out->files[i].path, line, GIT_MAX_PATH - 1);
            strncpy(out->files[i].sha, tab1, GIT_MAX_SHA - 1);
            out->files[i].status = atoi(tab2);
            out->files[i].size = atol(tab3);
            out->count++;
        }
next_line:
        line = next;
    }

    free(data);
    result.files_affected = out->count;
    return result;
}

GitResult git_save_state(r, state)
GitRepo *r;
GitFileList *state;
{
    GitResult result;
    char statepath[GIT_MAX_PATH];
    char *buf;
    int bufsize;
    int pos, i;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    /* allocate enough for header + all file entries */
    bufsize = 2048 + (state->count * (GIT_MAX_PATH + GIT_MAX_SHA + 32));
    buf = (char *)malloc(bufsize);
    if (!buf) {
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory saving state");
        return result;
    }

    pos = 0;
    pos += sprintf(buf + pos, "%s\n", r->owner);
    pos += sprintf(buf + pos, "%s\n", r->repo);
    pos += sprintf(buf + pos, "%s\n", r->branch);
    pos += sprintf(buf + pos, "%s\n", r->head_sha);
    pos += sprintf(buf + pos, "%s\n", r->tree_sha);
    pos += sprintf(buf + pos, "%d\n", state->count);

    for (i = 0; i < state->count; i++) {
        pos += sprintf(buf + pos, "%s\t%s\t%d\t%ld\n",
                       state->files[i].path,
                       state->files[i].sha,
                       state->files[i].status,
                       state->files[i].size);
    }

    sprintf(statepath, "%s/%s", r->local_path, GIT_STATE_FILE);
    if (git_write_file(statepath, buf, (long)pos) < 0) {
        result.code = GIT_ERR_DISK;
        sprintf(result.message, "Failed to write state file");
    }

    free(buf);
    result.files_affected = state->count;
    return result;
}

/*
 * Pending file format (.nextstep_git_pending):
 *   Line 1: commit message
 *   Line 2: author name
 *   Line 3: author email
 *   Line 4: file_count
 *   Lines 5+: path  (one staged file per line)
 */

GitResult git_load_pending(r, message_out, message_size)
GitRepo *r;
char *message_out;
int message_size;
{
    GitResult result;
    char pendpath[GIT_MAX_PATH];
    char *data;
    long fsize;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    if (message_out) message_out[0] = '\0';

    sprintf(pendpath, "%s/%s", r->local_path, GIT_PENDING_FILE);
    data = git_read_file(pendpath, &fsize);
    if (!data) {
        result.code = GIT_ERR_STATE;
        sprintf(result.message, "No pending commit");
        return result;
    }

    /* first line is the commit message */
    if (message_out) {
        char *nl = strchr(data, '\n');
        int len;
        if (nl) {
            len = nl - data;
        } else {
            len = strlen(data);
        }
        if (len >= message_size) len = message_size - 1;
        strncpy(message_out, data, len);
        message_out[len] = '\0';
    }

    free(data);
    return result;
}

GitResult git_save_pending(r, message)
GitRepo *r;
char *message;
{
    GitResult result;
    char pendpath[GIT_MAX_PATH];

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    sprintf(pendpath, "%s/%s", r->local_path, GIT_PENDING_FILE);
    if (git_write_file(pendpath, message, (long)strlen(message)) < 0) {
        result.code = GIT_ERR_DISK;
        sprintf(result.message, "Failed to write pending file");
    }

    return result;
}

GitResult git_clear_pending(r)
GitRepo *r;
{
    GitResult result;
    char pendpath[GIT_MAX_PATH];

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    sprintf(pendpath, "%s/%s", r->local_path, GIT_PENDING_FILE);
    unlink(pendpath);

    return result;
}

/* --- Repo initialization --- */

GitResult git_init_repo(r, owner, repo, branch, local_path, token)
GitRepo *r;
char *owner;
char *repo;
char *branch;
char *local_path;
char *token;
{
    GitResult result;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    memset(r, 0, sizeof(GitRepo));

    if (!owner || !repo || !local_path || !token) {
        result.code = GIT_ERR_PARAM;
        sprintf(result.message, "Missing required parameters");
        return result;
    }

    strncpy(r->owner, owner, GIT_MAX_OWNER - 1);
    strncpy(r->repo, repo, GIT_MAX_REPO - 1);
    if (branch && branch[0])
        strncpy(r->branch, branch, GIT_MAX_BRANCH - 1);
    else
        r->branch[0] = '\0';  /* empty = auto-detect */
    strncpy(r->local_path, local_path, GIT_MAX_PATH - 1);
    strncpy(r->token, token, GIT_MAX_TOKEN - 1);
    r->head_sha[0] = '\0';
    r->tree_sha[0] = '\0';

    return result;
}

/* --- Helper: send progress callback --- */

static int report_progress(fn, userdata, event, current, total, msg)
GitProgressFn fn;
void *userdata;
int event;
int current;
int total;
char *msg;
{
    GitProgress p;

    if (!fn) return 0;

    p.event = event;
    p.current = current;
    p.total = total;
    strncpy(p.message, msg, GIT_MAX_ERRMSG - 1);
    p.message[GIT_MAX_ERRMSG - 1] = '\0';

    return fn(&p, userdata);
}

/* --- Helper: extract parent directory from a file path --- */

static void path_dirname(filepath, dirout, dirout_size)
char *filepath;
char *dirout;
int dirout_size;
{
    char *last_slash;
    int len;

    strncpy(dirout, filepath, dirout_size - 1);
    dirout[dirout_size - 1] = '\0';

    last_slash = strrchr(dirout, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        dirout[0] = '\0';
    }
}

/*
 * Download a file from GitHub. Tries the Contents API first (works for
 * files under ~100KB). If the response isn't base64-encoded (large file),
 * falls back to the Git Blobs API using the blob SHA from the tree.
 *
 * Returns decoded file data (caller must free) and sets *out_len.
 * Returns NULL on failure.
 */
static char *gh_download_file(token, owner, repo, path, branch, blob_sha, out_len)
char *token;
char *owner;
char *repo;
char *path;
char *branch;
char *blob_sha;
int *out_len;
{
    char api_path[1024];
    char *response;
    char *val;
    int len;
    int http_status;
    int content_len;
    char *decoded;
    int decoded_len;
    int resp_size;

    *out_len = 0;

    /* Try Contents API first (small files) */
    response = (char *)malloc(GIT_RESPONSE_BUF);
    if (!response) return NULL;

    sprintf(api_path, "/repos/%s/%s/contents/%s?ref=%s",
            owner, repo, path, branch);
    http_status = gh_api_request(token, "GET", api_path, NULL,
                                 response, GIT_RESPONSE_BUF);

    if (http_status >= 200 && http_status < 300) {
        val = json_find_string(response, "encoding", &len);
        if (val && len >= 6 && strncmp(val, "base64", 6) == 0) {
            /* Got base64 content from Contents API */
            val = json_find_string(response, "content", &content_len);
            if (val && content_len > 0) {
                decoded = (char *)malloc(content_len + 1);
                if (decoded) {
                    decoded_len = gh_base64_decode(val, content_len,
                                                   decoded, content_len + 1);
                    free(response);
                    *out_len = decoded_len;
                    return decoded;
                }
            }
            free(response);
            return NULL;
        }
    }
    free(response);

    /* Contents API didn't return base64 — try Blobs API */
    if (!blob_sha || !blob_sha[0]) return NULL;

    /* Blobs API returns base64; allocate larger buffer.
     * GitHub blob limit is 100MB; we support up to ~1.3MB files here
     * (conservative for 32MB NeXTstation).
     */
    resp_size = 1024 * 1024 * 2;  /* 2MB */
    response = (char *)malloc(resp_size);
    if (!response) return NULL;

    sprintf(api_path, "/repos/%s/%s/git/blobs/%s",
            owner, repo, blob_sha);
    http_status = gh_api_request(token, "GET", api_path, NULL,
                                 response, resp_size);

    if (http_status < 200 || http_status >= 300) {
        free(response);
        return NULL;
    }

    val = json_find_string(response, "content", &content_len);
    if (!val || content_len == 0) {
        free(response);
        return NULL;
    }

    decoded = (char *)malloc(content_len + 1);
    if (!decoded) {
        free(response);
        return NULL;
    }

    decoded_len = gh_base64_decode(val, content_len, decoded, content_len + 1);
    free(response);
    *out_len = decoded_len;
    return decoded;
}

/* --- git_clone --- */

GitResult git_clone(r, progress_fn, userdata)
GitRepo *r;
GitProgressFn progress_fn;
void *userdata;
{
    GitResult result;
    char *response;
    char api_path[1024];
    int http_status;
    char *elem, *end;
    char *val;
    int len;
    typedef char PathBuf[GIT_MAX_PATH];
    typedef char ShaBuf[GIT_MAX_SHA];
    PathBuf *file_paths;
    ShaBuf *file_shas;
    int file_count;
    int i;
    GitFileList *state;
    char tree_sha_buf[GIT_MAX_SHA];
    char head_sha_buf[GIT_MAX_SHA];
    char type_buf[32];

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;
    file_count = 0;

    file_paths = (PathBuf *)malloc(GIT_MAX_FILES * sizeof(PathBuf));
    file_shas = (ShaBuf *)malloc(GIT_MAX_FILES * sizeof(ShaBuf));
    state = (GitFileList *)malloc(sizeof(GitFileList));
    if (!file_paths || !file_shas || !state) {
        if (file_paths) free(file_paths);
        if (file_shas) free(file_shas);
        if (state) free(state);
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    /* Create local directory */
    if (!git_is_directory(r->local_path)) {
        if (git_mkdir_p(r->local_path) < 0) {
            free(file_paths); free(file_shas); free(state);
            result.code = GIT_ERR_DISK;
            sprintf(result.message, "Cannot create directory: %s", r->local_path);
            return result;
        }
    }

    response = (char *)malloc(GIT_RESPONSE_BUF);
    if (!response) {
        free(file_paths); free(file_shas); free(state);
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    /* Step 0: detect default branch if none specified */
    if (report_progress(progress_fn, userdata, GIT_PROGRESS_START,
                        0, 0, "Getting branch info...") < 0) {
        free(response); free(file_paths); free(file_shas); free(state);
        result.code = GIT_ERR_PARAM;
        sprintf(result.message, "Cancelled");
        return result;
    }

    if (r->branch[0] == '\0') {
        /* No branch specified — ask the repo API for the default */
        sprintf(api_path, "/repos/%s/%s", r->owner, r->repo);
        http_status = gh_api_request(r->token, "GET", api_path, NULL,
                                     response, GIT_RESPONSE_BUF);
        if (http_status >= 200 && http_status < 300) {
            val = json_find_string(response, "default_branch", &len);
            if (val && len > 0 && len < GIT_MAX_BRANCH) {
                strncpy(r->branch, val, len);
                r->branch[len] = '\0';
            }
        }
        if (r->branch[0] == '\0') {
            strncpy(r->branch, "main", GIT_MAX_BRANCH - 1);
        }
    }

    sprintf(api_path, "/repos/%s/%s/git/refs/heads/%s",
            r->owner, r->repo, r->branch);
    http_status = gh_api_request(r->token, "GET", api_path, NULL,
                                 response, GIT_RESPONSE_BUF);

    if (http_status < 200 || http_status >= 300) {
        free(response); free(file_paths); free(file_shas); free(state);
        result.code = (http_status == 404) ? GIT_ERR_NOTFOUND :
                      (http_status == 401 || http_status == 403) ? GIT_ERR_AUTH :
                      GIT_ERR_NETWORK;
        sprintf(result.message, "Failed to get branch ref (HTTP %d)", http_status);
        return result;
    }

    /* Extract commit SHA from ref response: object.sha */
    val = json_find_string(response, "sha", &len);
    if (!val || len < 1) {
        free(response); free(file_paths); free(file_shas); free(state);
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message, "Could not parse branch ref");
        return result;
    }
    if (len > GIT_MAX_SHA - 1) len = GIT_MAX_SHA - 1;
    strncpy(head_sha_buf, val, len);
    head_sha_buf[len] = '\0';

    /* Step 2: Get the tree (recursive) */
    report_progress(progress_fn, userdata, GIT_PROGRESS_STATUS,
                    0, 0, "Getting file tree...");

    sprintf(api_path, "/repos/%s/%s/git/trees/%s?recursive=1",
            r->owner, r->repo, r->branch);
    http_status = gh_api_request(r->token, "GET", api_path, NULL,
                                 response, GIT_RESPONSE_BUF);

    if (http_status < 200 || http_status >= 300) {
        free(response); free(file_paths); free(file_shas); free(state);
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message, "Failed to get file tree (HTTP %d)", http_status);
        return result;
    }

    /* Get tree SHA */
    val = json_find_string(response, "sha", &len);
    if (val && len > 0 && len < GIT_MAX_SHA) {
        strncpy(tree_sha_buf, val, len);
        tree_sha_buf[len] = '\0';
    } else {
        tree_sha_buf[0] = '\0';
    }

    /* Parse tree entries */
    {
        char *tree_arr;

        /* find the "tree" array */
        tree_arr = strstr(response, "\"tree\"");
        if (!tree_arr) {
            free(response); free(file_paths); free(file_shas); free(state);
            result.code = GIT_ERR_NETWORK;
            sprintf(result.message, "No tree array in response");
            return result;
        }

        elem = json_array_first(tree_arr, &end);
        while (elem && file_count < GIT_MAX_FILES) {
            /* Check type == "blob" (skip trees/dirs) */
            val = json_find_string(elem, "type", &len);
            if (val && len > 0) {
                if (len > (int)sizeof(type_buf) - 1) len = sizeof(type_buf) - 1;
                strncpy(type_buf, val, len);
                type_buf[len] = '\0';

                if (strcmp(type_buf, "blob") == 0) {
                    /* Get path */
                    val = json_find_string(elem, "path", &len);
                    if (val && len > 0 && len < GIT_MAX_PATH) {
                        json_unescape(val, len, file_paths[file_count],
                                      GIT_MAX_PATH);

                        /* Get SHA */
                        val = json_find_string(elem, "sha", &len);
                        if (val && len > 0 && len < GIT_MAX_SHA) {
                            strncpy(file_shas[file_count], val, len);
                            file_shas[file_count][len] = '\0';
                        } else {
                            file_shas[file_count][0] = '\0';
                        }

                        file_count++;
                    }
                }
            }

            elem = json_array_next(elem, &end);
        }
    }

    if (file_count == 0) {
        free(response);
        strcpy(r->head_sha, head_sha_buf);
        strcpy(r->tree_sha, tree_sha_buf);

        /* Save state with empty file list */
        state->count = 0;
        git_save_state(r, state);

        free(file_paths); free(file_shas); free(state);
        result.code = GIT_OK;
        sprintf(result.message, "Cloned empty repository");
        return result;
    }

    /* Step 3: Download each file */
    state->count = 0;

    for (i = 0; i < file_count; i++) {
        char filepath[GIT_MAX_PATH];
        char dirpath[GIT_MAX_PATH];
        char progress_msg[GIT_MAX_ERRMSG];
        char *decoded;
        int decoded_len;

        sprintf(progress_msg, "Downloading %s (%d/%d)",
                file_paths[i], i + 1, file_count);

        if (report_progress(progress_fn, userdata, GIT_PROGRESS_DOWNLOAD,
                            i + 1, file_count, progress_msg) < 0) {
            free(response); free(file_paths); free(file_shas); free(state);
            result.code = GIT_ERR_PARAM;
            sprintf(result.message, "Cancelled at file %d/%d", i + 1, file_count);
            return result;
        }

        /* Download file via Contents API or Blobs API fallback */
        decoded = gh_download_file(r->token, r->owner, r->repo,
                                    file_paths[i], r->branch,
                                    file_shas[i], &decoded_len);
        if (!decoded) continue;

        /* Create parent directories */
        sprintf(filepath, "%s/%s", r->local_path, file_paths[i]);
        path_dirname(filepath, dirpath, GIT_MAX_PATH);
        if (dirpath[0] && !git_is_directory(dirpath)) {
            git_mkdir_p(dirpath);
        }

        /* Write file */
        if (git_write_file(filepath, decoded, (long)decoded_len) == 0) {
            /* Add to state */
            if (state->count < GIT_MAX_FILES) {
                char local_hash[GIT_MAX_SHA];
                strncpy(state->files[state->count].path, file_paths[i],
                        GIT_MAX_PATH - 1);
                content_hash(decoded, (long)decoded_len, local_hash);
                strncpy(state->files[state->count].sha, local_hash,
                        GIT_MAX_SHA - 1);
                state->files[state->count].status = GIT_STATUS_TRACKED;
                state->files[state->count].size = (long)decoded_len;
                state->count++;
            }
            result.files_affected++;
        }

        free(decoded);
    }

    /* Save repo state */
    strcpy(r->head_sha, head_sha_buf);
    strcpy(r->tree_sha, tree_sha_buf);
    git_save_state(r, state);

    free(response);
    free(file_paths);
    free(file_shas);
    free(state);

    report_progress(progress_fn, userdata, GIT_PROGRESS_DONE,
                    file_count, file_count, "Clone complete");

    sprintf(result.message, "Cloned %d files from %s/%s",
            result.files_affected, r->owner, r->repo);
    return result;
}

/* --- git_status --- */

GitResult git_status(r, out)
GitRepo *r;
GitFileList *out;
{
    GitResult result;
    GitFileList *tracked;
    typedef char PathBuf2[GIT_MAX_PATH];
    PathBuf2 *disk_paths;
    int disk_count;
    int i, j, found;
    char filepath[GIT_MAX_PATH];
    char *data;
    long fsize;
    char hash[GIT_MAX_SHA];

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;
    out->count = 0;

    tracked = (GitFileList *)malloc(sizeof(GitFileList));
    disk_paths = (PathBuf2 *)malloc(GIT_MAX_FILES * sizeof(PathBuf2));
    if (!tracked || !disk_paths) {
        if (tracked) free(tracked);
        if (disk_paths) free(disk_paths);
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    /* Load tracked state */
    result = git_load_state(r, tracked);
    if (result.code != GIT_OK) {
        free(tracked); free(disk_paths);
        return result;
    }

    /* Scan local directory */
    disk_count = git_scan_directory(r->local_path, disk_paths, GIT_MAX_FILES);

    /* Check each tracked file */
    for (i = 0; i < tracked->count && out->count < GIT_MAX_FILES; i++) {
        sprintf(filepath, "%s/%s", r->local_path, tracked->files[i].path);

        if (!git_is_file(filepath)) {
            /* File was deleted */
            memcpy(&out->files[out->count], &tracked->files[i],
                   sizeof(GitFileEntry));
            out->files[out->count].status = GIT_STATUS_DELETED;
            out->count++;
            result.files_affected++;
        } else {
            /* File exists - check if modified */
            data = git_read_file(filepath, &fsize);
            if (data) {
                content_hash(data, fsize, hash);
                free(data);

                if (fsize != tracked->files[i].size ||
                    strcmp(hash, tracked->files[i].sha) != 0) {
                    /* Modified locally - but preserve staged status */
                    memcpy(&out->files[out->count], &tracked->files[i],
                           sizeof(GitFileEntry));
                    if (tracked->files[i].status == GIT_STATUS_STAGED) {
                        out->files[out->count].status = GIT_STATUS_STAGED;
                    } else {
                        out->files[out->count].status = GIT_STATUS_MODIFIED;
                    }
                    out->files[out->count].size = fsize;
                    out->count++;
                    result.files_affected++;
                } else if (tracked->files[i].status == GIT_STATUS_STAGED) {
                    /* Staged but content matches - still show as staged */
                    memcpy(&out->files[out->count], &tracked->files[i],
                           sizeof(GitFileEntry));
                    out->count++;
                    result.files_affected++;
                }
                /* else: clean/tracked, don't add to status output */
            }
        }
    }

    /* Check for new (untracked) files on disk */
    for (i = 0; i < disk_count && out->count < GIT_MAX_FILES; i++) {
        found = 0;
        for (j = 0; j < tracked->count; j++) {
            if (strcmp(disk_paths[i], tracked->files[j].path) == 0) {
                found = 1;
                break;
            }
        }

        if (!found) {
            strncpy(out->files[out->count].path, disk_paths[i],
                    GIT_MAX_PATH - 1);
            out->files[out->count].sha[0] = '\0';
            out->files[out->count].status = GIT_STATUS_NEW;

            sprintf(filepath, "%s/%s", r->local_path, disk_paths[i]);
            data = git_read_file(filepath, &fsize);
            if (data) {
                out->files[out->count].size = fsize;
                free(data);
            } else {
                out->files[out->count].size = 0;
            }

            out->count++;
            result.files_affected++;
        }
    }

    free(tracked);
    free(disk_paths);

    sprintf(result.message, "%d changed files", result.files_affected);
    return result;
}

/* --- git_add --- */

GitResult git_add(r, path, state)
GitRepo *r;
char *path;
GitFileList *state;
{
    GitResult result;
    int i, found;
    char filepath[GIT_MAX_PATH];
    char *data;
    long fsize;
    char hash[GIT_MAX_SHA];

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    if (!path || !path[0]) {
        result.code = GIT_ERR_PARAM;
        sprintf(result.message, "No file path specified");
        return result;
    }

    /* Check file exists on disk */
    sprintf(filepath, "%s/%s", r->local_path, path);
    if (!git_is_file(filepath)) {
        /* Check if it's a tracked file that was deleted */
        found = 0;
        for (i = 0; i < state->count; i++) {
            if (strcmp(state->files[i].path, path) == 0) {
                state->files[i].status = GIT_STATUS_STAGED;
                found = 1;
                result.files_affected = 1;
                break;
            }
        }
        if (!found) {
            result.code = GIT_ERR_NOTFOUND;
            sprintf(result.message, "File not found: %s", path);
        } else {
            sprintf(result.message, "Staged deleted file: %s", path);
        }
        return result;
    }

    /* Read file to compute hash */
    data = git_read_file(filepath, &fsize);
    if (!data) {
        result.code = GIT_ERR_DISK;
        sprintf(result.message, "Cannot read file: %s", path);
        return result;
    }
    content_hash(data, fsize, hash);
    free(data);

    /* Find in state or add new entry */
    found = 0;
    for (i = 0; i < state->count; i++) {
        if (strcmp(state->files[i].path, path) == 0) {
            state->files[i].status = GIT_STATUS_STAGED;
            state->files[i].size = fsize;
            found = 1;
            break;
        }
    }

    if (!found) {
        /* New file, add to state */
        if (state->count >= GIT_MAX_FILES) {
            result.code = GIT_ERR_TOOLARGE;
            sprintf(result.message, "Too many files tracked");
            return result;
        }
        i = state->count;
        strncpy(state->files[i].path, path, GIT_MAX_PATH - 1);
        state->files[i].path[GIT_MAX_PATH - 1] = '\0';
        strncpy(state->files[i].sha, hash, GIT_MAX_SHA - 1);
        state->files[i].status = GIT_STATUS_STAGED;
        state->files[i].size = fsize;
        state->count++;
    }

    result.files_affected = 1;
    sprintf(result.message, "Staged: %s", path);

    /* Save state */
    git_save_state(r, state);

    return result;
}

/* --- git_add_all --- */

GitResult git_add_all(r, state)
GitRepo *r;
GitFileList *state;
{
    GitResult result;
    GitFileList *status_list;
    int i;
    GitResult add_result;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    status_list = (GitFileList *)malloc(sizeof(GitFileList));
    if (!status_list) {
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    /* Get current status */
    result = git_status(r, status_list);
    if (result.code != GIT_OK) {
        free(status_list);
        return result;
    }

    if (status_list->count == 0) {
        free(status_list);
        result.code = GIT_ERR_NOCHANGES;
        sprintf(result.message, "No changes to stage");
        return result;
    }

    /* Stage each changed file */
    for (i = 0; i < status_list->count; i++) {
        add_result = git_add(r, status_list->files[i].path, state);
        if (add_result.code == GIT_OK) {
            result.files_affected += add_result.files_affected;
        }
    }

    free(status_list);
    sprintf(result.message, "Staged %d files", result.files_affected);
    return result;
}

/* --- git_commit --- */

GitResult git_commit(r, message, author_name, author_email, state)
GitRepo *r;
char *message;
char *author_name;
char *author_email;
GitFileList *state;
{
    GitResult result;
    int i;
    int staged_count;
    char *pending_buf;
    int pending_buf_size;
    int pos;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    if (!message || !message[0]) {
        result.code = GIT_ERR_PARAM;
        sprintf(result.message, "Commit message is required");
        return result;
    }

    /* Count staged files */
    staged_count = 0;
    for (i = 0; i < state->count; i++) {
        if (state->files[i].status == GIT_STATUS_STAGED) {
            staged_count++;
        }
    }

    if (staged_count == 0) {
        result.code = GIT_ERR_NOCHANGES;
        sprintf(result.message, "No files staged for commit");
        return result;
    }

    pending_buf_size = GIT_MAX_MSG + GIT_MAX_AUTHOR * 2 +
                       GIT_MAX_FILES * GIT_MAX_PATH + 256;
    pending_buf = (char *)malloc(pending_buf_size);
    if (!pending_buf) {
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    /* Build pending commit data */
    pos = 0;
    pos += sprintf(pending_buf + pos, "%s\n", message);
    pos += sprintf(pending_buf + pos, "%s\n",
                   author_name ? author_name : "NeXTSTEP User");
    pos += sprintf(pending_buf + pos, "%s\n",
                   author_email ? author_email : "user@nextstep.local");
    pos += sprintf(pending_buf + pos, "%d\n", staged_count);

    for (i = 0; i < state->count; i++) {
        if (state->files[i].status == GIT_STATUS_STAGED) {
            pos += sprintf(pending_buf + pos, "%s\n", state->files[i].path);
        }
    }

    /* Save pending commit */
    result = git_save_pending(r, pending_buf);
    free(pending_buf);
    if (result.code != GIT_OK) return result;

    result.files_affected = staged_count;
    sprintf(result.message, "Committed %d files (pending push)", staged_count);
    return result;
}

/* --- git_push --- */

GitResult git_push(r, state, progress_fn, userdata)
GitRepo *r;
GitFileList *state;
GitProgressFn progress_fn;
void *userdata;
{
    GitResult result;
    char *response;
    char *pending_data;
    long pending_size;
    char pending_path[GIT_MAX_PATH];
    char api_path[1024];
    int http_status;
    char *val;
    int len;

    /* Pending file parsed fields */
    char commit_msg[GIT_MAX_MSG];
    char author_name[GIT_MAX_AUTHOR];
    char author_email[GIT_MAX_AUTHOR];
    int staged_count;
    typedef char PushPathBuf[GIT_MAX_PATH];
    PushPathBuf *staged_paths;

    /* For building blobs/tree/commit */
    typedef char PushShaBuf[GIT_MAX_SHA];
    PushShaBuf *blob_shas;
    char *post_body;
    int post_size;
    char *file_data;
    long file_size;
    char *b64_data;
    int b64_len;
    char *escaped;
    int escaped_size;
    char esc_msg[GIT_MAX_MSG * 2];
    char new_tree_sha[GIT_MAX_SHA];
    char new_commit_sha[GIT_MAX_SHA];

    /* Parsing pending file */
    char *line, *next_line;
    int line_num;
    int i;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;
    staged_count = 0;

    staged_paths = (PushPathBuf *)malloc(GIT_MAX_FILES * sizeof(PushPathBuf));
    blob_shas = (PushShaBuf *)malloc(GIT_MAX_FILES * sizeof(PushShaBuf));
    if (!staged_paths || !blob_shas) {
        if (staged_paths) free(staged_paths);
        if (blob_shas) free(blob_shas);
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    /* Load pending commit */
    sprintf(pending_path, "%s/%s", r->local_path, GIT_PENDING_FILE);
    pending_data = git_read_file(pending_path, &pending_size);
    if (!pending_data) {
        free(staged_paths); free(blob_shas);
        result.code = GIT_ERR_STATE;
        sprintf(result.message, "No pending commit to push (run commit first)");
        return result;
    }

    /* Parse pending file */
    commit_msg[0] = '\0';
    author_name[0] = '\0';
    author_email[0] = '\0';
    line = pending_data;
    line_num = 0;

    while (line && *line) {
        next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }

        /* strip CR */
        {
            int ll = strlen(line);
            if (ll > 0 && line[ll-1] == '\r')
                line[ll-1] = '\0';
        }

        line_num++;

        if (line_num == 1) {
            strncpy(commit_msg, line, GIT_MAX_MSG - 1);
        } else if (line_num == 2) {
            strncpy(author_name, line, GIT_MAX_AUTHOR - 1);
        } else if (line_num == 3) {
            strncpy(author_email, line, GIT_MAX_AUTHOR - 1);
        } else if (line_num == 4) {
            staged_count = atoi(line);
        } else {
            /* staged file paths */
            i = line_num - 5;
            if (i >= 0 && i < GIT_MAX_FILES && line[0]) {
                strncpy(staged_paths[i], line, GIT_MAX_PATH - 1);
                staged_paths[i][GIT_MAX_PATH - 1] = '\0';
            }
        }

        line = next_line;
    }
    free(pending_data);

    if (staged_count <= 0) {
        free(staged_paths); free(blob_shas);
        result.code = GIT_ERR_NOCHANGES;
        sprintf(result.message, "No files in pending commit");
        return result;
    }

    response = (char *)malloc(GIT_RESPONSE_BUF);
    if (!response) {
        free(staged_paths); free(blob_shas);
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    report_progress(progress_fn, userdata, GIT_PROGRESS_START,
                    0, staged_count, "Starting push...");

    /* Step 1: Create blobs for each staged file */
    for (i = 0; i < staged_count; i++) {
        char filepath[GIT_MAX_PATH];
        char progress_msg[GIT_MAX_ERRMSG];
        int is_deleted;

        sprintf(progress_msg, "Uploading %s (%d/%d)",
                staged_paths[i], i + 1, staged_count);
        if (report_progress(progress_fn, userdata, GIT_PROGRESS_UPLOAD,
                            i + 1, staged_count, progress_msg) < 0) {
            free(response); free(staged_paths); free(blob_shas);
            result.code = GIT_ERR_PARAM;
            sprintf(result.message, "Cancelled");
            return result;
        }

        /* Check if file is deleted */
        sprintf(filepath, "%s/%s", r->local_path, staged_paths[i]);
        is_deleted = !git_is_file(filepath);

        if (is_deleted) {
            /* Mark with empty sha to signal deletion in tree */
            blob_shas[i][0] = '\0';
            continue;
        }

        /* Read file */
        file_data = git_read_file(filepath, &file_size);
        if (!file_data) {
            free(response); free(staged_paths); free(blob_shas);
            result.code = GIT_ERR_DISK;
            sprintf(result.message, "Cannot read file: %s", staged_paths[i]);
            return result;
        }

        /* Base64 encode */
        b64_data = (char *)malloc(file_size * 2 + 4);
        if (!b64_data) {
            free(file_data);
            free(response); free(staged_paths); free(blob_shas);
            result.code = GIT_ERR_MEMORY;
            sprintf(result.message, "Out of memory encoding %s", staged_paths[i]);
            return result;
        }
        b64_len = gh_base64_encode(file_data, (int)file_size, b64_data,
                                   (int)(file_size * 2 + 4));
        free(file_data);

        /* POST /repos/{owner}/{repo}/git/blobs */
        /* Body: {"content":"<base64>","encoding":"base64"} */
        post_size = b64_len + 256;
        post_body = (char *)malloc(post_size);
        if (!post_body) {
            free(b64_data);
            free(response); free(staged_paths); free(blob_shas);
            result.code = GIT_ERR_MEMORY;
            sprintf(result.message, "Out of memory");
            return result;
        }

        sprintf(post_body, "{\"content\":\"%s\",\"encoding\":\"base64\"}", b64_data);
        free(b64_data);

        sprintf(api_path, "/repos/%s/%s/git/blobs", r->owner, r->repo);
        http_status = gh_api_request(r->token, "POST", api_path, post_body,
                                     response, GIT_RESPONSE_BUF);
        free(post_body);

        if (http_status < 200 || http_status >= 300) {
            result.code = GIT_ERR_NETWORK;
            sprintf(result.message, "Blob fail %s (HTTP %d): %.200s",
                    staged_paths[i], http_status, response);
            free(response); free(staged_paths); free(blob_shas);
            return result;
        }

        /* Extract blob SHA */
        val = json_find_string(response, "sha", &len);
        if (!val || len < 1) {
            free(response); free(staged_paths); free(blob_shas);
            result.code = GIT_ERR_NETWORK;
            sprintf(result.message, "No SHA in blob response for %s",
                    staged_paths[i]);
            return result;
        }
        if (len > GIT_MAX_SHA - 1) len = GIT_MAX_SHA - 1;
        strncpy(blob_shas[i], val, len);
        blob_shas[i][len] = '\0';
    }

    /* Step 2: Create tree */
    report_progress(progress_fn, userdata, GIT_PROGRESS_STATUS,
                    0, 0, "Creating tree...");

    /* Build tree entries JSON */
    {
        int tree_size;
        int pos;
        char esc_path[GIT_MAX_PATH * 2];

        tree_size = 512 + staged_count * (GIT_MAX_PATH * 2 + GIT_MAX_SHA + 128);
        post_body = (char *)malloc(tree_size);
        if (!post_body) {
            free(response); free(staged_paths); free(blob_shas);
            result.code = GIT_ERR_MEMORY;
            sprintf(result.message, "Out of memory building tree");
            return result;
        }

        pos = sprintf(post_body, "{\"base_tree\":\"%s\",\"tree\":[",
                      r->tree_sha);

        for (i = 0; i < staged_count; i++) {
            if (i > 0) {
                post_body[pos++] = ',';
            }

            json_escape(staged_paths[i], esc_path, sizeof(esc_path));

            if (blob_shas[i][0] == '\0') {
                /* Deleted file: set sha to null to remove from tree */
                pos += sprintf(post_body + pos,
                    "{\"path\":\"%s\",\"mode\":\"100644\",\"type\":\"blob\",\"sha\":null}",
                    esc_path);
            } else {
                pos += sprintf(post_body + pos,
                    "{\"path\":\"%s\",\"mode\":\"100644\",\"type\":\"blob\",\"sha\":\"%s\"}",
                    esc_path, blob_shas[i]);
            }
        }

        pos += sprintf(post_body + pos, "]}");

        sprintf(api_path, "/repos/%s/%s/git/trees", r->owner, r->repo);
        http_status = gh_api_request(r->token, "POST", api_path, post_body,
                                     response, GIT_RESPONSE_BUF);
        free(post_body);

        if (http_status < 200 || http_status >= 300) {
            free(response); free(staged_paths); free(blob_shas);
            result.code = GIT_ERR_NETWORK;
            sprintf(result.message, "Failed to create tree (HTTP %d)", http_status);
            return result;
        }

        /* Extract new tree SHA */
        val = json_find_string(response, "sha", &len);
        if (!val || len < 1) {
            free(response); free(staged_paths); free(blob_shas);
            result.code = GIT_ERR_NETWORK;
            sprintf(result.message, "No SHA in tree response");
            return result;
        }
        if (len > GIT_MAX_SHA - 1) len = GIT_MAX_SHA - 1;
        strncpy(new_tree_sha, val, len);
        new_tree_sha[len] = '\0';
    }

    /* Step 3: Create commit */
    report_progress(progress_fn, userdata, GIT_PROGRESS_STATUS,
                    0, 0, "Creating commit...");

    json_escape(commit_msg, esc_msg, sizeof(esc_msg));

    {
        char esc_name[GIT_MAX_AUTHOR * 2];
        char esc_email[GIT_MAX_AUTHOR * 2];

        json_escape(author_name, esc_name, sizeof(esc_name));
        json_escape(author_email, esc_email, sizeof(esc_email));

        post_size = strlen(esc_msg) + strlen(esc_name) + strlen(esc_email) +
                    GIT_MAX_SHA * 2 + 512;
        post_body = (char *)malloc(post_size);
        if (!post_body) {
            free(response); free(staged_paths); free(blob_shas);
            result.code = GIT_ERR_MEMORY;
            sprintf(result.message, "Out of memory creating commit");
            return result;
        }

        sprintf(post_body,
            "{\"message\":\"%s\","
            "\"tree\":\"%s\","
            "\"parents\":[\"%s\"],"
            "\"author\":{\"name\":\"%s\",\"email\":\"%s\"}}",
            esc_msg, new_tree_sha, r->head_sha, esc_name, esc_email);
    }

    sprintf(api_path, "/repos/%s/%s/git/commits", r->owner, r->repo);
    http_status = gh_api_request(r->token, "POST", api_path, post_body,
                                 response, GIT_RESPONSE_BUF);
    free(post_body);

    if (http_status < 200 || http_status >= 300) {
        free(response); free(staged_paths); free(blob_shas);
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message, "Failed to create commit (HTTP %d)", http_status);
        return result;
    }

    /* Extract commit SHA */
    val = json_find_string(response, "sha", &len);
    if (!val || len < 1) {
        free(response); free(staged_paths); free(blob_shas);
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message, "No SHA in commit response");
        return result;
    }
    if (len > GIT_MAX_SHA - 1) len = GIT_MAX_SHA - 1;
    strncpy(new_commit_sha, val, len);
    new_commit_sha[len] = '\0';

    /* Step 4: Update ref */
    report_progress(progress_fn, userdata, GIT_PROGRESS_STATUS,
                    0, 0, "Updating branch ref...");

    post_body = (char *)malloc(256);
    if (!post_body) {
        free(response); free(staged_paths); free(blob_shas);
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }
    sprintf(post_body, "{\"sha\":\"%s\"}", new_commit_sha);

    sprintf(api_path, "/repos/%s/%s/git/refs/heads/%s",
            r->owner, r->repo, r->branch);
    http_status = gh_api_request(r->token, "PATCH", api_path, post_body,
                                 response, GIT_RESPONSE_BUF);
    free(post_body);

    if (http_status < 200 || http_status >= 300) {
        free(response); free(staged_paths); free(blob_shas);
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message, "Failed to update ref (HTTP %d)", http_status);
        return result;
    }

    free(response);

    /* Update local state */
    strcpy(r->head_sha, new_commit_sha);
    strcpy(r->tree_sha, new_tree_sha);

    /* Update tracked file state: staged -> tracked, deleted -> removed */
    {
        int j;
        for (i = 0; i < staged_count; i++) {
            for (j = 0; j < state->count; j++) {
                if (strcmp(state->files[j].path, staged_paths[i]) == 0) {
                    if (state->files[j].status == GIT_STATUS_STAGED) {
                        /* Check if the file was deleted */
                        char fp[GIT_MAX_PATH];
                        sprintf(fp, "%s/%s", r->local_path, staged_paths[i]);
                        if (!git_is_file(fp)) {
                            /* Remove from state by shifting */
                            int k;
                            for (k = j; k < state->count - 1; k++) {
                                memcpy(&state->files[k], &state->files[k+1],
                                       sizeof(GitFileEntry));
                            }
                            state->count--;
                            j--; /* re-check this index */
                        } else {
                            /* Mark as tracked, keep local content hash */
                            state->files[j].status = GIT_STATUS_TRACKED;
                        }
                    }
                    break;
                }
            }
        }
    }

    git_save_state(r, state);
    git_clear_pending(r);

    free(staged_paths);
    free(blob_shas);

    report_progress(progress_fn, userdata, GIT_PROGRESS_DONE,
                    staged_count, staged_count, "Push complete");

    result.files_affected = staged_count;
    sprintf(result.message, "Pushed %d files to %s/%s:%s",
            staged_count, r->owner, r->repo, r->branch);
    return result;
}

/* --- git_pull --- */

GitResult git_pull(r, progress_fn, userdata)
GitRepo *r;
GitProgressFn progress_fn;
void *userdata;
{
    GitResult result;
    GitFileList *tracked;
    char *response;
    char api_path[1024];
    int http_status;
    char *val;
    int len;
    char remote_head[GIT_MAX_SHA];
    char remote_tree_sha[GIT_MAX_SHA];
    char *elem, *end;
    int i, j;
    int updated;
    char type_buf[32];

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    tracked = (GitFileList *)malloc(sizeof(GitFileList));
    if (!tracked) {
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    /* Load current state */
    result = git_load_state(r, tracked);
    if (result.code != GIT_OK) {
        free(tracked);
        return result;
    }

    response = (char *)malloc(GIT_RESPONSE_BUF);
    if (!response) {
        free(tracked);
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    report_progress(progress_fn, userdata, GIT_PROGRESS_START,
                    0, 0, "Checking for updates...");

    /* Get current remote HEAD */
    sprintf(api_path, "/repos/%s/%s/git/refs/heads/%s",
            r->owner, r->repo, r->branch);
    http_status = gh_api_request(r->token, "GET", api_path, NULL,
                                 response, GIT_RESPONSE_BUF);

    if (http_status < 200 || http_status >= 300) {
        free(response); free(tracked);
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message, "Failed to get branch ref (HTTP %d)", http_status);
        return result;
    }

    val = json_find_string(response, "sha", &len);
    if (!val || len < 1) {
        free(response); free(tracked);
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message, "Could not parse branch ref");
        return result;
    }
    if (len > GIT_MAX_SHA - 1) len = GIT_MAX_SHA - 1;
    strncpy(remote_head, val, len);
    remote_head[len] = '\0';

    /* Check if already up to date */
    if (strcmp(remote_head, r->head_sha) == 0) {
        free(response); free(tracked);
        sprintf(result.message, "Already up to date");
        return result;
    }

    /* Get remote tree */
    report_progress(progress_fn, userdata, GIT_PROGRESS_STATUS,
                    0, 0, "Getting remote tree...");

    sprintf(api_path, "/repos/%s/%s/git/trees/%s?recursive=1",
            r->owner, r->repo, remote_head);
    http_status = gh_api_request(r->token, "GET", api_path, NULL,
                                 response, GIT_RESPONSE_BUF);

    if (http_status < 200 || http_status >= 300) {
        free(response); free(tracked);
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message, "Failed to get remote tree (HTTP %d)", http_status);
        return result;
    }

    /* Get tree SHA */
    val = json_find_string(response, "sha", &len);
    if (val && len > 0 && len < GIT_MAX_SHA) {
        strncpy(remote_tree_sha, val, len);
        remote_tree_sha[len] = '\0';
    } else {
        remote_tree_sha[0] = '\0';
    }

    /* Compare remote tree entries with tracked state, download changed files */
    updated = 0;
    {
        char *tree_arr;
        char remote_path[GIT_MAX_PATH];
        char remote_sha[GIT_MAX_SHA];
        int found;

        tree_arr = strstr(response, "\"tree\"");
        if (!tree_arr) {
            free(response); free(tracked);
            result.code = GIT_ERR_NETWORK;
            sprintf(result.message, "No tree array in response");
            return result;
        }

        elem = json_array_first(tree_arr, &end);
        while (elem) {
            /* Only process blobs */
            val = json_find_string(elem, "type", &len);
            if (!val || len < 4 || strncmp(val, "blob", 4) != 0) {
                elem = json_array_next(elem, &end);
                continue;
            }

            /* Get path */
            val = json_find_string(elem, "path", &len);
            if (!val || len < 1 || len >= GIT_MAX_PATH) {
                elem = json_array_next(elem, &end);
                continue;
            }
            json_unescape(val, len, remote_path, GIT_MAX_PATH);

            /* Get SHA */
            val = json_find_string(elem, "sha", &len);
            if (!val || len < 1) {
                elem = json_array_next(elem, &end);
                continue;
            }
            if (len > GIT_MAX_SHA - 1) len = GIT_MAX_SHA - 1;
            strncpy(remote_sha, val, len);
            remote_sha[len] = '\0';

            /* Check if this file changed */
            found = 0;
            for (j = 0; j < tracked->count; j++) {
                if (strcmp(tracked->files[j].path, remote_path) == 0) {
                    found = 1;
                    if (strcmp(tracked->files[j].sha, remote_sha) == 0) {
                        /* unchanged */
                        break;
                    }
                    /* SHA differs - need to download */
                    break;
                }
            }

            if (found && j < tracked->count &&
                strcmp(tracked->files[j].sha, remote_sha) == 0) {
                /* File unchanged, skip */
                elem = json_array_next(elem, &end);
                continue;
            }

            /* Need to download this file (new or changed) */
            {
                char filepath[GIT_MAX_PATH];
                char dirpath[GIT_MAX_PATH];
                char progress_msg[GIT_MAX_ERRMSG];
                char *decoded;
                int decoded_len;

                updated++;

                sprintf(progress_msg, "Updating %s", remote_path);
                report_progress(progress_fn, userdata, GIT_PROGRESS_DOWNLOAD,
                                updated, 0, progress_msg);

                /* Download file via Contents API or Blobs API fallback */
                decoded = gh_download_file(r->token, r->owner, r->repo,
                                            remote_path, r->branch,
                                            remote_sha, &decoded_len);
                if (!decoded) {
                    elem = json_array_next(elem, &end);
                    continue;
                }

                /* Create dirs and write file */
                sprintf(filepath, "%s/%s", r->local_path, remote_path);
                path_dirname(filepath, dirpath, GIT_MAX_PATH);
                if (dirpath[0] && !git_is_directory(dirpath)) {
                    git_mkdir_p(dirpath);
                }

                if (git_write_file(filepath, decoded, (long)decoded_len) == 0) {
                    /* Update tracked state with local content hash */
                    char local_hash[GIT_MAX_SHA];
                    content_hash(decoded, (long)decoded_len, local_hash);
                    if (found && j < tracked->count) {
                        strncpy(tracked->files[j].sha, local_hash,
                                GIT_MAX_SHA - 1);
                        tracked->files[j].status = GIT_STATUS_TRACKED;
                        tracked->files[j].size = (long)decoded_len;
                    } else {
                        /* New file */
                        if (tracked->count < GIT_MAX_FILES) {
                            strncpy(tracked->files[tracked->count].path,
                                    remote_path, GIT_MAX_PATH - 1);
                            strncpy(tracked->files[tracked->count].sha,
                                    local_hash, GIT_MAX_SHA - 1);
                            tracked->files[tracked->count].status =
                                GIT_STATUS_TRACKED;
                            tracked->files[tracked->count].size =
                                (long)decoded_len;
                            tracked->count++;
                        }
                    }
                    result.files_affected++;
                }

                free(decoded);
            }

            elem = json_array_next(elem, &end);
        }
    }

    /* TODO: detect files deleted on remote (in tracked but not in remote tree) */

    /* Update state */
    strcpy(r->head_sha, remote_head);
    strcpy(r->tree_sha, remote_tree_sha);
    git_save_state(r, tracked);

    free(response);
    free(tracked);

    report_progress(progress_fn, userdata, GIT_PROGRESS_DONE,
                    0, 0, "Pull complete");

    if (result.files_affected == 0) {
        sprintf(result.message, "Already up to date (ref updated)");
    } else {
        sprintf(result.message, "Updated %d files from %s/%s:%s",
                result.files_affected, r->owner, r->repo, r->branch);
    }
    return result;
}

/* --- Branch operations --- */

/*
 * List remote branches. Calls GET /repos/:o/:r/branches?per_page=50
 * and marks the current branch (from r->branch) with is_current=1.
 */
GitResult git_branch_list(r, out)
GitRepo *r;
GitBranchList *out;
{
    GitResult result;
    char *response;
    char api_path[1024];
    int http_status;
    char *elem, *end;
    char *val;
    int len;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;
    out->count = 0;

    response = (char *)malloc(GIT_RESPONSE_BUF);
    if (!response) {
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    sprintf(api_path, "/repos/%s/%s/branches?per_page=%d",
            r->owner, r->repo, GIT_MAX_BRANCHES);
    http_status = gh_api_request(r->token, "GET", api_path, NULL,
                                 response, GIT_RESPONSE_BUF);

    if (http_status < 200 || http_status >= 300) {
        free(response);
        result.code = (http_status == 404) ? GIT_ERR_NOTFOUND :
                      (http_status == 401 || http_status == 403) ? GIT_ERR_AUTH :
                      GIT_ERR_NETWORK;
        sprintf(result.message, "Failed to list branches (HTTP %d)", http_status);
        return result;
    }

    /* Parse JSON array of branch objects */
    elem = json_array_first(response, &end);
    while (elem && out->count < GIT_MAX_BRANCHES) {
        val = json_find_string(elem, "name", &len);
        if (val && len > 0 && len < GIT_MAX_BRANCH) {
            json_unescape(val, len, out->branches[out->count].name,
                          GIT_MAX_BRANCH);

            /* Get commit SHA (nested: commit.sha) */
            val = json_find_string(elem, "sha", &len);
            if (val && len > 0 && len < GIT_MAX_SHA) {
                strncpy(out->branches[out->count].sha, val, len);
                out->branches[out->count].sha[len] = '\0';
            } else {
                out->branches[out->count].sha[0] = '\0';
            }

            out->branches[out->count].is_current =
                (strcmp(out->branches[out->count].name, r->branch) == 0);

            out->count++;
        }
        elem = json_array_next(elem, &end);
    }

    free(response);

    result.files_affected = out->count;
    sprintf(result.message, "%d branches", out->count);
    return result;
}

/*
 * Create a new branch. POSTs to /repos/:o/:r/git/refs with
 * {"ref":"refs/heads/<name>","sha":"<from_sha>"}.
 * If from_sha is NULL, uses r->head_sha.
 */
GitResult git_branch_create(r, name, from_sha)
GitRepo *r;
char *name;
char *from_sha;
{
    GitResult result;
    char *response;
    char api_path[1024];
    char post_body[512];
    int http_status;
    char *sha;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    if (!name || !name[0]) {
        result.code = GIT_ERR_PARAM;
        sprintf(result.message, "Branch name required");
        return result;
    }

    sha = (from_sha && from_sha[0]) ? from_sha : r->head_sha;
    if (!sha[0]) {
        result.code = GIT_ERR_STATE;
        sprintf(result.message, "No HEAD SHA available");
        return result;
    }

    response = (char *)malloc(GIT_RESPONSE_BUF);
    if (!response) {
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    sprintf(api_path, "/repos/%s/%s/git/refs", r->owner, r->repo);
    sprintf(post_body, "{\"ref\":\"refs/heads/%s\",\"sha\":\"%s\"}", name, sha);

    http_status = gh_api_request(r->token, "POST", api_path, post_body,
                                 response, GIT_RESPONSE_BUF);

    free(response);

    if (http_status == 201) {
        sprintf(result.message, "Branch '%s' created", name);
    } else if (http_status == 422) {
        result.code = GIT_ERR_CONFLICT;
        sprintf(result.message, "Branch '%s' already exists", name);
    } else {
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message, "Failed to create branch (HTTP %d)", http_status);
    }

    return result;
}

/*
 * Switch to a different branch. Steps:
 * 1. Verify branch exists (GET ref)
 * 2. Get remote tree for target branch
 * 3. Compare with current tracked files, download changed/new files
 * 4. Update r->branch, r->head_sha, r->tree_sha, save state
 *
 * Refuses to switch if there are uncommitted staged changes.
 */
GitResult git_branch_switch(r, name, fl, progress_fn, userdata)
GitRepo *r;
char *name;
GitFileList *fl;
GitProgressFn progress_fn;
void *userdata;
{
    GitResult result;
    char *response;
    char api_path[1024];
    int http_status;
    char *val;
    int len;
    char new_head[GIT_MAX_SHA];
    char new_tree_sha[GIT_MAX_SHA];
    char *elem, *end;
    int i, j;
    int updated;
    char type_buf[32];
    typedef char PathBuf[GIT_MAX_PATH];
    typedef char ShaBuf[GIT_MAX_SHA];
    PathBuf *remote_paths;
    ShaBuf *remote_shas;
    int remote_count;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    if (!name || !name[0]) {
        result.code = GIT_ERR_PARAM;
        sprintf(result.message, "Branch name required");
        return result;
    }

    /* Already on this branch? */
    if (strcmp(r->branch, name) == 0) {
        sprintf(result.message, "Already on branch '%s'", name);
        return result;
    }

    /* Check for staged changes */
    for (i = 0; i < fl->count; i++) {
        if (fl->files[i].status == GIT_STATUS_STAGED) {
            result.code = GIT_ERR_CONFLICT;
            sprintf(result.message,
                "Cannot switch branches: you have staged changes.\n"
                "Commit or unstage them first.");
            return result;
        }
    }

    response = (char *)malloc(GIT_RESPONSE_BUF);
    if (!response) {
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    remote_paths = (PathBuf *)malloc(GIT_MAX_FILES * sizeof(PathBuf));
    remote_shas = (ShaBuf *)malloc(GIT_MAX_FILES * sizeof(ShaBuf));
    if (!remote_paths || !remote_shas) {
        free(response);
        if (remote_paths) free(remote_paths);
        if (remote_shas) free(remote_shas);
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    /* Step 1: Get target branch ref */
    report_progress(progress_fn, userdata, GIT_PROGRESS_START,
                    0, 0, "Getting branch info...");

    sprintf(api_path, "/repos/%s/%s/git/refs/heads/%s",
            r->owner, r->repo, name);
    http_status = gh_api_request(r->token, "GET", api_path, NULL,
                                 response, GIT_RESPONSE_BUF);

    if (http_status < 200 || http_status >= 300) {
        free(response); free(remote_paths); free(remote_shas);
        result.code = (http_status == 404) ? GIT_ERR_NOTFOUND : GIT_ERR_NETWORK;
        sprintf(result.message, "Branch '%s' not found (HTTP %d)", name, http_status);
        return result;
    }

    val = json_find_string(response, "sha", &len);
    if (!val || len < 1) {
        free(response); free(remote_paths); free(remote_shas);
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message, "Could not parse branch ref");
        return result;
    }
    if (len > GIT_MAX_SHA - 1) len = GIT_MAX_SHA - 1;
    strncpy(new_head, val, len);
    new_head[len] = '\0';

    /* Step 2: Get target branch tree */
    report_progress(progress_fn, userdata, GIT_PROGRESS_STATUS,
                    0, 0, "Getting file tree...");

    sprintf(api_path, "/repos/%s/%s/git/trees/%s?recursive=1",
            r->owner, r->repo, new_head);
    http_status = gh_api_request(r->token, "GET", api_path, NULL,
                                 response, GIT_RESPONSE_BUF);

    if (http_status < 200 || http_status >= 300) {
        free(response); free(remote_paths); free(remote_shas);
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message, "Failed to get tree (HTTP %d)", http_status);
        return result;
    }

    /* Get tree SHA */
    val = json_find_string(response, "sha", &len);
    if (val && len > 0 && len < GIT_MAX_SHA) {
        strncpy(new_tree_sha, val, len);
        new_tree_sha[len] = '\0';
    } else {
        new_tree_sha[0] = '\0';
    }

    /* Parse remote tree entries */
    remote_count = 0;
    {
        char *tree_arr;

        tree_arr = strstr(response, "\"tree\"");
        if (!tree_arr) {
            free(response); free(remote_paths); free(remote_shas);
            result.code = GIT_ERR_NETWORK;
            sprintf(result.message, "No tree array in response");
            return result;
        }

        elem = json_array_first(tree_arr, &end);
        while (elem && remote_count < GIT_MAX_FILES) {
            val = json_find_string(elem, "type", &len);
            if (val && len > 0) {
                if (len > (int)sizeof(type_buf) - 1) len = sizeof(type_buf) - 1;
                strncpy(type_buf, val, len);
                type_buf[len] = '\0';

                if (strcmp(type_buf, "blob") == 0) {
                    val = json_find_string(elem, "path", &len);
                    if (val && len > 0 && len < GIT_MAX_PATH) {
                        json_unescape(val, len, remote_paths[remote_count],
                                      GIT_MAX_PATH);

                        val = json_find_string(elem, "sha", &len);
                        if (val && len > 0 && len < GIT_MAX_SHA) {
                            strncpy(remote_shas[remote_count], val, len);
                            remote_shas[remote_count][len] = '\0';
                        } else {
                            remote_shas[remote_count][0] = '\0';
                        }
                        remote_count++;
                    }
                }
            }
            elem = json_array_next(elem, &end);
        }
    }

    /* Step 3: Download changed/new files */
    updated = 0;
    for (i = 0; i < remote_count; i++) {
        int found;
        int match_idx;

        /* Check if file exists in current state with same SHA */
        found = 0;
        match_idx = -1;
        for (j = 0; j < fl->count; j++) {
            if (strcmp(fl->files[j].path, remote_paths[i]) == 0) {
                found = 1;
                match_idx = j;
                break;
            }
        }

        /* If file exists locally and SHA matches the remote blob SHA,
         * we still need to check — the state stores content_hash not
         * git blob SHA. So for branch switch, always download files
         * that don't exist locally or whose blob SHA differs from
         * what we had. For efficiency, if the remote blob SHA matches
         * what the other branch had, skip it.
         *
         * Simple approach: download if file is new or if blob SHA
         * changed from what we last pulled. We don't store blob SHAs
         * in state (we store content hashes), so for branch switch
         * we re-download all files. This is correct but slow.
         *
         * Optimization: only download if file doesn't exist on disk. */
        if (found) {
            /* File exists in our state — check if it exists on disk
             * and skip download if so (the pull after switch will
             * catch any diffs). For a proper switch, we should compare
             * blob SHAs, but we don't store them. Download all for
             * correctness on first implementation. */
        }

        {
            char filepath[GIT_MAX_PATH];
            char dirpath[GIT_MAX_PATH];
            char progress_msg[GIT_MAX_ERRMSG];
            char *decoded;
            int decoded_len;

            updated++;
            sprintf(progress_msg, "%s (%d/%d)",
                    remote_paths[i], updated, remote_count);
            report_progress(progress_fn, userdata, GIT_PROGRESS_DOWNLOAD,
                            updated, remote_count, progress_msg);

            decoded = gh_download_file(r->token, r->owner, r->repo,
                                        remote_paths[i], name,
                                        remote_shas[i], &decoded_len);
            if (!decoded) continue;

            sprintf(filepath, "%s/%s", r->local_path, remote_paths[i]);
            path_dirname(filepath, dirpath, GIT_MAX_PATH);
            if (dirpath[0] && !git_is_directory(dirpath)) {
                git_mkdir_p(dirpath);
            }

            if (git_write_file(filepath, decoded, (long)decoded_len) == 0) {
                char local_hash[GIT_MAX_SHA];
                content_hash(decoded, (long)decoded_len, local_hash);
                if (found && match_idx >= 0) {
                    strncpy(fl->files[match_idx].sha, local_hash,
                            GIT_MAX_SHA - 1);
                    fl->files[match_idx].status = GIT_STATUS_TRACKED;
                    fl->files[match_idx].size = (long)decoded_len;
                } else {
                    if (fl->count < GIT_MAX_FILES) {
                        strncpy(fl->files[fl->count].path,
                                remote_paths[i], GIT_MAX_PATH - 1);
                        strncpy(fl->files[fl->count].sha,
                                local_hash, GIT_MAX_SHA - 1);
                        fl->files[fl->count].status = GIT_STATUS_TRACKED;
                        fl->files[fl->count].size = (long)decoded_len;
                        fl->count++;
                    }
                }
                result.files_affected++;
            }

            free(decoded);
        }
    }

    /* Step 4: Remove local files not in remote tree */
    for (j = fl->count - 1; j >= 0; j--) {
        int in_remote;

        in_remote = 0;
        for (i = 0; i < remote_count; i++) {
            if (strcmp(fl->files[j].path, remote_paths[i]) == 0) {
                in_remote = 1;
                break;
            }
        }

        if (!in_remote) {
            char filepath[GIT_MAX_PATH];
            sprintf(filepath, "%s/%s", r->local_path, fl->files[j].path);
            unlink(filepath);

            /* Remove from file list by shifting */
            for (i = j; i < fl->count - 1; i++) {
                fl->files[i] = fl->files[i + 1];
            }
            fl->count--;
        }
    }

    /* Update repo state */
    strncpy(r->branch, name, GIT_MAX_BRANCH - 1);
    r->branch[GIT_MAX_BRANCH - 1] = '\0';
    strcpy(r->head_sha, new_head);
    strcpy(r->tree_sha, new_tree_sha);
    git_save_state(r, fl);

    free(response);
    free(remote_paths);
    free(remote_shas);

    report_progress(progress_fn, userdata, GIT_PROGRESS_DONE,
                    0, 0, "Branch switch complete");

    sprintf(result.message, "Switched to branch '%s' (%d files updated)",
            name, result.files_affected);
    return result;
}

/*
 * Delete a remote branch. Calls DELETE /repos/:o/:r/git/refs/heads/<name>.
 * Refuses to delete the current branch.
 */
GitResult git_branch_delete(r, name)
GitRepo *r;
char *name;
{
    GitResult result;
    char *response;
    char api_path[1024];
    int http_status;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    if (!name || !name[0]) {
        result.code = GIT_ERR_PARAM;
        sprintf(result.message, "Branch name required");
        return result;
    }

    /* Refuse to delete current branch */
    if (strcmp(r->branch, name) == 0) {
        result.code = GIT_ERR_PARAM;
        sprintf(result.message, "Cannot delete the current branch '%s'", name);
        return result;
    }

    response = (char *)malloc(GIT_RESPONSE_BUF);
    if (!response) {
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    sprintf(api_path, "/repos/%s/%s/git/refs/heads/%s",
            r->owner, r->repo, name);
    http_status = gh_api_request(r->token, "DELETE", api_path, NULL,
                                 response, GIT_RESPONSE_BUF);

    free(response);

    if (http_status == 204) {
        sprintf(result.message, "Deleted branch '%s'", name);
    } else if (http_status == 404) {
        result.code = GIT_ERR_NOTFOUND;
        sprintf(result.message, "Branch '%s' not found", name);
    } else if (http_status == 422) {
        result.code = GIT_ERR_PARAM;
        sprintf(result.message,
            "Cannot delete branch '%s' (may be protected)", name);
    } else {
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message, "Failed to delete branch (HTTP %d)", http_status);
    }

    return result;
}

/* --- Merge operations --- */

/*
 * Server-side merge via POST /repos/:o/:r/merges.
 * Merges head_branch into the current branch (r->branch).
 * HTTP 201 = success, 204 = already up to date, 409 = conflict.
 * If message is NULL, GitHub generates a default merge commit message.
 */
GitMergeResult git_merge(r, head_branch, message)
GitRepo *r;
char *head_branch;
char *message;
{
    GitMergeResult result;
    char *response;
    char api_path[1024];
    char *post_body;
    int post_len;
    int http_status;
    char *val;
    int len;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.sha[0] = '\0';
    result.conflicts = 0;

    if (!head_branch || !head_branch[0]) {
        result.code = GIT_ERR_PARAM;
        sprintf(result.message, "Branch name required");
        return result;
    }

    /* Can't merge a branch into itself */
    if (strcmp(r->branch, head_branch) == 0) {
        result.code = GIT_ERR_PARAM;
        sprintf(result.message,
            "Cannot merge '%s' into itself", head_branch);
        return result;
    }

    response = (char *)malloc(GIT_RESPONSE_BUF);
    if (!response) {
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    /* Build POST body */
    post_body = (char *)malloc(GIT_MAX_MSG + 256);
    if (!post_body) {
        free(response);
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    if (message && message[0]) {
        char escaped_msg[GIT_MAX_MSG];
        json_escape(message, escaped_msg, GIT_MAX_MSG);
        sprintf(post_body,
            "{\"base\":\"%s\",\"head\":\"%s\",\"commit_message\":\"%s\"}",
            r->branch, head_branch, escaped_msg);
    } else {
        sprintf(post_body,
            "{\"base\":\"%s\",\"head\":\"%s\"}",
            r->branch, head_branch);
    }

    sprintf(api_path, "/repos/%s/%s/merges", r->owner, r->repo);
    http_status = gh_api_request(r->token, "POST", api_path, post_body,
                                 response, GIT_RESPONSE_BUF);

    free(post_body);

    if (http_status == 201) {
        /* Merge successful — extract new commit SHA */
        val = json_find_string(response, "sha", &len);
        if (val && len > 0 && len < GIT_MAX_SHA) {
            strncpy(result.sha, val, len);
            result.sha[len] = '\0';
        }
        sprintf(result.message,
            "Merged '%s' into '%s'", head_branch, r->branch);
    } else if (http_status == 204) {
        /* Already up to date — no merge needed */
        sprintf(result.message,
            "Already up to date: '%s' is already merged into '%s'",
            head_branch, r->branch);
    } else if (http_status == 409) {
        /* Merge conflict */
        result.code = GIT_ERR_CONFLICT;
        result.conflicts = 1;
        sprintf(result.message,
            "Merge conflict: cannot merge '%s' into '%s' automatically",
            head_branch, r->branch);
    } else if (http_status == 404) {
        result.code = GIT_ERR_NOTFOUND;
        sprintf(result.message,
            "Branch '%s' not found", head_branch);
    } else {
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message,
            "Merge failed (HTTP %d)", http_status);
    }

    free(response);
    return result;
}

/* --- Tag operations --- */

/*
 * List tags. Calls GET /repos/:o/:r/tags?per_page=50.
 */
GitResult git_tag_list(r, out)
GitRepo *r;
GitTagList *out;
{
    GitResult result;
    char *response;
    char api_path[1024];
    int http_status;
    char *elem, *end;
    char *val;
    int len;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;
    out->count = 0;

    response = (char *)malloc(GIT_RESPONSE_BUF);
    if (!response) {
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    sprintf(api_path, "/repos/%s/%s/tags?per_page=%d",
            r->owner, r->repo, GIT_MAX_TAGS);
    http_status = gh_api_request(r->token, "GET", api_path, NULL,
                                 response, GIT_RESPONSE_BUF);

    if (http_status < 200 || http_status >= 300) {
        free(response);
        result.code = (http_status == 404) ? GIT_ERR_NOTFOUND :
                      GIT_ERR_NETWORK;
        sprintf(result.message, "Failed to list tags (HTTP %d)",
                http_status);
        return result;
    }

    /* Parse JSON array of tag objects */
    elem = json_array_first(response, &end);
    while (elem && out->count < GIT_MAX_TAGS) {
        val = json_find_string(elem, "name", &len);
        if (val && len > 0 && len < GIT_MAX_BRANCH) {
            json_unescape(val, len, out->tags[out->count].name,
                          GIT_MAX_BRANCH);

            /* Get commit SHA (nested: commit.sha) */
            val = json_find_string(elem, "sha", &len);
            if (val && len > 0 && len < GIT_MAX_SHA) {
                strncpy(out->tags[out->count].sha, val, len);
                out->tags[out->count].sha[len] = '\0';
            } else {
                out->tags[out->count].sha[0] = '\0';
            }

            out->tags[out->count].message[0] = '\0';
            out->tags[out->count].is_annotated = 0;
            out->count++;
        }
        elem = json_array_next(elem, &end);
    }

    free(response);

    result.files_affected = out->count;
    sprintf(result.message, "%d tags", out->count);
    return result;
}

/*
 * Create a tag. If message is NULL, creates a lightweight tag.
 * If message is provided, creates an annotated tag.
 *
 * Lightweight: POST /repos/:o/:r/git/refs
 *   {"ref":"refs/tags/<name>","sha":"<head_sha>"}
 *
 * Annotated: first POST /repos/:o/:r/git/tags to create tag object,
 *   then POST /repos/:o/:r/git/refs to point ref at it.
 */
GitResult git_tag_create(r, name, message)
GitRepo *r;
char *name;
char *message;
{
    GitResult result;
    char *response;
    char api_path[1024];
    char *post_body;
    int http_status;
    char *val;
    int len;
    char tag_sha[GIT_MAX_SHA];

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    if (!name || !name[0]) {
        result.code = GIT_ERR_PARAM;
        sprintf(result.message, "Tag name required");
        return result;
    }

    if (!r->head_sha[0]) {
        result.code = GIT_ERR_STATE;
        sprintf(result.message, "No HEAD SHA available");
        return result;
    }

    response = (char *)malloc(GIT_RESPONSE_BUF);
    post_body = (char *)malloc(GIT_MAX_MSG + 512);
    if (!response || !post_body) {
        if (response) free(response);
        if (post_body) free(post_body);
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    if (message && message[0]) {
        /* Annotated tag: create tag object first */
        char escaped_msg[GIT_MAX_MSG];
        json_escape(message, escaped_msg, GIT_MAX_MSG);

        sprintf(api_path, "/repos/%s/%s/git/tags",
                r->owner, r->repo);
        sprintf(post_body,
            "{\"tag\":\"%s\",\"message\":\"%s\","
            "\"object\":\"%s\",\"type\":\"commit\"}",
            name, escaped_msg, r->head_sha);

        http_status = gh_api_request(r->token, "POST", api_path,
                                     post_body, response,
                                     GIT_RESPONSE_BUF);

        if (http_status != 201) {
            free(response);
            free(post_body);
            result.code = (http_status == 422) ? GIT_ERR_CONFLICT :
                          GIT_ERR_NETWORK;
            sprintf(result.message,
                "Failed to create tag object (HTTP %d)", http_status);
            return result;
        }

        /* Extract tag object SHA */
        val = json_find_string(response, "sha", &len);
        if (val && len > 0 && len < GIT_MAX_SHA) {
            strncpy(tag_sha, val, len);
            tag_sha[len] = '\0';
        } else {
            free(response);
            free(post_body);
            result.code = GIT_ERR_NETWORK;
            sprintf(result.message, "Could not parse tag object SHA");
            return result;
        }

        /* Create ref pointing to tag object */
        sprintf(api_path, "/repos/%s/%s/git/refs",
                r->owner, r->repo);
        sprintf(post_body,
            "{\"ref\":\"refs/tags/%s\",\"sha\":\"%s\"}",
            name, tag_sha);
    } else {
        /* Lightweight tag: just create the ref */
        strcpy(tag_sha, r->head_sha);
        sprintf(api_path, "/repos/%s/%s/git/refs",
                r->owner, r->repo);
        sprintf(post_body,
            "{\"ref\":\"refs/tags/%s\",\"sha\":\"%s\"}",
            name, r->head_sha);
    }

    http_status = gh_api_request(r->token, "POST", api_path,
                                 post_body, response, GIT_RESPONSE_BUF);

    free(response);
    free(post_body);

    if (http_status == 201) {
        if (message && message[0]) {
            sprintf(result.message,
                "Created annotated tag '%s'", name);
        } else {
            sprintf(result.message,
                "Created lightweight tag '%s'", name);
        }
    } else if (http_status == 422) {
        result.code = GIT_ERR_CONFLICT;
        sprintf(result.message, "Tag '%s' already exists", name);
    } else {
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message,
            "Failed to create tag ref (HTTP %d)", http_status);
    }

    return result;
}

/* --- Compare operations --- */

/*
 * Compare two branches via GET /repos/:o/:r/compare/:base...:head.
 * Parses status, ahead/behind counts, and file list with patches.
 * Patches are heap-allocated and truncated to GIT_MAX_PATCH bytes.
 */
GitResult git_compare(r, base, head, out)
GitRepo *r;
char *base;
char *head;
GitCompareResult *out;
{
    GitResult result;
    char *response;
    char api_path[1024];
    int http_status;
    char *val;
    int len;
    char *elem, *end;
    char *files_arr;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;

    out->status[0] = '\0';
    out->ahead_by = 0;
    out->behind_by = 0;
    out->total_commits = 0;
    out->files = NULL;
    out->file_count = 0;

    if (!base || !base[0] || !head || !head[0]) {
        result.code = GIT_ERR_PARAM;
        sprintf(result.message, "Both base and head are required");
        return result;
    }

    response = (char *)malloc(GIT_RESPONSE_BUF);
    if (!response) {
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    sprintf(api_path, "/repos/%s/%s/compare/%s...%s",
            r->owner, r->repo, base, head);
    http_status = gh_api_request(r->token, "GET", api_path, NULL,
                                 response, GIT_RESPONSE_BUF);

    if (http_status < 200 || http_status >= 300) {
        free(response);
        result.code = (http_status == 404) ? GIT_ERR_NOTFOUND :
                      GIT_ERR_NETWORK;
        sprintf(result.message,
            "Compare failed (HTTP %d)", http_status);
        return result;
    }

    /* Parse status */
    val = json_find_string(response, "status", &len);
    if (val && len > 0 && len < 16) {
        strncpy(out->status, val, len);
        out->status[len] = '\0';
    }

    /* Parse ahead/behind counts */
    out->ahead_by = (int)json_find_number(response, "ahead_by");
    out->behind_by = (int)json_find_number(response, "behind_by");
    out->total_commits = (int)json_find_number(response, "total_commits");
    if (out->ahead_by < 0) out->ahead_by = 0;
    if (out->behind_by < 0) out->behind_by = 0;
    if (out->total_commits < 0) out->total_commits = 0;

    /* Parse files array */
    files_arr = strstr(response, "\"files\"");
    if (!files_arr) {
        /* No files section — possibly identical */
        free(response);
        sprintf(result.message, "%s: %d commit(s), 0 files",
                out->status, out->total_commits);
        return result;
    }

    /* Count files first */
    {
        int count;
        char *tmp_elem, *tmp_end;

        count = 0;
        tmp_elem = json_array_first(files_arr, &tmp_end);
        while (tmp_elem && count < GIT_MAX_COMPARE_FILES) {
            count++;
            tmp_elem = json_array_next(tmp_elem, &tmp_end);
        }

        if (count == 0) {
            free(response);
            sprintf(result.message, "%s: %d commit(s), 0 files",
                    out->status, out->total_commits);
            return result;
        }

        out->files = (GitCompareFile *)malloc(
            count * sizeof(GitCompareFile));
        if (!out->files) {
            free(response);
            result.code = GIT_ERR_MEMORY;
            sprintf(result.message, "Out of memory");
            return result;
        }
        memset(out->files, 0, count * sizeof(GitCompareFile));
    }

    /* Parse each file entry */
    elem = json_array_first(files_arr, &end);
    while (elem && out->file_count < GIT_MAX_COMPARE_FILES) {
        GitCompareFile *f;

        f = &out->files[out->file_count];

        /* filename */
        val = json_find_string(elem, "filename", &len);
        if (val && len > 0 && len < GIT_MAX_PATH) {
            json_unescape(val, len, f->filename, GIT_MAX_PATH);
        } else {
            f->filename[0] = '\0';
        }

        /* status */
        val = json_find_string(elem, "status", &len);
        if (val && len > 0 && len < 16) {
            strncpy(f->status, val, len);
            f->status[len] = '\0';
        } else {
            strcpy(f->status, "changed");
        }

        /* additions / deletions */
        f->additions = (int)json_find_number(elem, "additions");
        f->deletions = (int)json_find_number(elem, "deletions");
        if (f->additions < 0) f->additions = 0;
        if (f->deletions < 0) f->deletions = 0;

        /* patch text (heap-allocated, truncated) */
        val = json_find_string(elem, "patch", &len);
        if (val && len > 0) {
            int patch_len;

            patch_len = (len > GIT_MAX_PATCH - 1) ?
                        GIT_MAX_PATCH - 1 : len;
            f->patch = (char *)malloc(patch_len + 1);
            if (f->patch) {
                json_unescape(val, patch_len, f->patch, patch_len + 1);
            }
        } else {
            f->patch = NULL;
        }

        out->file_count++;
        elem = json_array_next(elem, &end);
    }

    free(response);

    result.files_affected = out->file_count;
    sprintf(result.message, "%s: %d commit(s), %d file(s) changed",
            out->status, out->total_commits, out->file_count);
    return result;
}

/*
 * Free heap-allocated compare result.
 */
void git_compare_free(result)
GitCompareResult *result;
{
    int i;

    if (result->files) {
        for (i = 0; i < result->file_count; i++) {
            if (result->files[i].patch) {
                free(result->files[i].patch);
                result->files[i].patch = NULL;
            }
        }
        free(result->files);
        result->files = NULL;
    }
    result->file_count = 0;
}

/* --- git_log --- */

GitResult git_log(r, max_count, out)
GitRepo *r;
int max_count;
GitCommitLog *out;
{
    GitResult result;
    char *response;
    char api_path[1024];
    int http_status;
    char *elem, *end;
    char *val;
    int len;
    int count;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;
    out->count = 0;

    if (max_count <= 0) max_count = 10;
    if (max_count > GIT_MAX_COMMITS) max_count = GIT_MAX_COMMITS;

    response = (char *)malloc(GIT_RESPONSE_BUF);
    if (!response) {
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    sprintf(api_path, "/repos/%s/%s/commits?sha=%s&per_page=%d",
            r->owner, r->repo, r->branch, max_count);
    http_status = gh_api_request(r->token, "GET", api_path, NULL,
                                 response, GIT_RESPONSE_BUF);

    if (http_status < 200 || http_status >= 300) {
        free(response);
        result.code = GIT_ERR_NETWORK;
        sprintf(result.message, "Failed to get commits (HTTP %d)", http_status);
        return result;
    }

    count = 0;
    elem = json_array_first(response, &end);
    while (elem && count < max_count) {
        /* SHA */
        val = json_find_string(elem, "sha", &len);
        if (val && len > 0) {
            if (len > GIT_MAX_SHA - 1) len = GIT_MAX_SHA - 1;
            strncpy(out->commits[count].sha, val, len);
            out->commits[count].sha[len] = '\0';
        } else {
            out->commits[count].sha[0] = '\0';
        }

        /* Message - nested under "commit" object */
        {
            char *commit_obj;
            commit_obj = strstr(elem, "\"commit\"");
            if (commit_obj) {
                val = json_find_string(commit_obj, "message", &len);
                if (val && len > 0) {
                    if (len > GIT_MAX_MSG - 1) len = GIT_MAX_MSG - 1;
                    json_unescape(val, len, out->commits[count].message,
                                  GIT_MAX_MSG);
                } else {
                    out->commits[count].message[0] = '\0';
                }

                /* Author name - nested under commit.author */
                {
                    char *author_obj;
                    author_obj = strstr(commit_obj, "\"author\"");
                    if (author_obj) {
                        val = json_find_string(author_obj, "name", &len);
                        if (val && len > 0) {
                            if (len > GIT_MAX_AUTHOR - 1)
                                len = GIT_MAX_AUTHOR - 1;
                            json_unescape(val, len,
                                          out->commits[count].author,
                                          GIT_MAX_AUTHOR);
                        } else {
                            out->commits[count].author[0] = '\0';
                        }

                        val = json_find_string(author_obj, "date", &len);
                        if (val && len > 0) {
                            if (len > GIT_MAX_DATE - 1)
                                len = GIT_MAX_DATE - 1;
                            json_unescape(val, len,
                                          out->commits[count].date,
                                          GIT_MAX_DATE);
                        } else {
                            out->commits[count].date[0] = '\0';
                        }
                    }
                }
            } else {
                out->commits[count].message[0] = '\0';
                out->commits[count].author[0] = '\0';
                out->commits[count].date[0] = '\0';
            }
        }

        count++;
        elem = json_array_next(elem, &end);
    }

    out->count = count;
    free(response);

    result.files_affected = count;
    sprintf(result.message, "%d commits", count);
    return result;
}

/* --- git_diff --- */

GitResult git_diff(r, path, state, out, progress_fn, userdata)
GitRepo *r;
char *path;
GitFileList *state;
GitDiffResult *out;
GitProgressFn progress_fn;
void *userdata;
{
    GitResult result;
    GitFileList *status_list;
    int i, j;
    int diff_count;
    char *response;

    result.code = GIT_OK;
    result.message[0] = '\0';
    result.files_affected = 0;
    out->entries = NULL;
    out->count = 0;

    status_list = (GitFileList *)malloc(sizeof(GitFileList));
    if (!status_list) {
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    /* Get status to find changed files */
    result = git_status(r, status_list);
    if (result.code != GIT_OK) {
        free(status_list);
        return result;
    }

    if (status_list->count == 0) {
        free(status_list);
        sprintf(result.message, "No changes");
        return result;
    }

    /* If path specified, filter to that file */
    if (path && path[0]) {
        diff_count = 0;
        for (i = 0; i < status_list->count; i++) {
            if (strcmp(status_list->files[i].path, path) == 0) {
                diff_count = 1;
                /* move to position 0 if not already */
                if (i != 0) {
                    memcpy(&status_list->files[0], &status_list->files[i],
                           sizeof(GitFileEntry));
                }
                break;
            }
        }
        if (diff_count == 0) {
            free(status_list);
            result.code = GIT_ERR_NOTFOUND;
            sprintf(result.message, "File not found in changes: %s", path);
            return result;
        }
        status_list->count = 1;
    }

    /* Allocate diff entries */
    out->entries = (GitDiffEntry *)malloc(
        status_list->count * sizeof(GitDiffEntry));
    if (!out->entries) {
        free(status_list);
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    response = (char *)malloc(GIT_RESPONSE_BUF);
    if (!response) {
        free(out->entries);
        out->entries = NULL;
        free(status_list);
        result.code = GIT_ERR_MEMORY;
        sprintf(result.message, "Out of memory");
        return result;
    }

    diff_count = 0;

    for (i = 0; i < status_list->count; i++) {
        char filepath[GIT_MAX_PATH];
        char *local_data;
        long local_size;
        GitDiffEntry *de;

        de = &out->entries[diff_count];
        memset(de, 0, sizeof(GitDiffEntry));
        strncpy(de->path, status_list->files[i].path, GIT_MAX_PATH - 1);
        de->status = status_list->files[i].status;

        /* Read local file */
        sprintf(filepath, "%s/%s", r->local_path, status_list->files[i].path);
        if (git_is_file(filepath)) {
            local_data = git_read_file(filepath, &local_size);
            if (local_data) {
                de->new_content = local_data; /* caller must free via git_diff_free */
                de->new_size = (int)local_size;
            }
        }

        /* Fetch remote content for tracked/modified files */
        if (status_list->files[i].status == GIT_STATUS_MODIFIED ||
            status_list->files[i].status == GIT_STATUS_DELETED) {
            char api_path[1024];
            int http_status;
            char *val;
            int len;

            sprintf(api_path, "/repos/%s/%s/contents/%s?ref=%s",
                    r->owner, r->repo, status_list->files[i].path, r->branch);

            report_progress(progress_fn, userdata, GIT_PROGRESS_DOWNLOAD,
                            i + 1, status_list->count,
                            "Fetching remote version...");

            http_status = gh_api_request(r->token, "GET", api_path, NULL,
                                         response, GIT_RESPONSE_BUF);

            if (http_status >= 200 && http_status < 300) {
                val = json_find_string(response, "encoding", &len);
                if (val && len >= 6 && strncmp(val, "base64", 6) == 0) {
                    int content_len;
                    val = json_find_string(response, "content", &content_len);
                    if (val && content_len > 0) {
                        char *decoded;
                        int decoded_len;

                        decoded = (char *)malloc(content_len + 1);
                        if (decoded) {
                            decoded_len = gh_base64_decode(val, content_len,
                                                          decoded,
                                                          content_len + 1);
                            de->old_content = decoded;
                            de->old_size = decoded_len;
                        }
                    }
                }
            }
        }

        diff_count++;
    }

    free(response);
    free(status_list);

    out->count = diff_count;
    result.files_affected = diff_count;
    sprintf(result.message, "%d files differ", diff_count);
    return result;
}

/* --- git_diff_free --- */

void git_diff_free(diff)
GitDiffResult *diff;
{
    int i;

    if (!diff) return;

    if (diff->entries) {
        for (i = 0; i < diff->count; i++) {
            if (diff->entries[i].old_content) {
                free(diff->entries[i].old_content);
                diff->entries[i].old_content = NULL;
            }
            if (diff->entries[i].new_content) {
                free(diff->entries[i].new_content);
                diff->entries[i].new_content = NULL;
            }
        }
        free(diff->entries);
        diff->entries = NULL;
    }
    diff->count = 0;
}
