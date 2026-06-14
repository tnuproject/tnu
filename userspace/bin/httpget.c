/*
 * httpget - Simple HTTP Client for TNU
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>

#define HTTP_PORT 80
#define HTTPS_PORT 443
#define BUFFER_SIZE 4096
#define MAX_URL_LEN 1024
#define MAX_HOST_LEN 256

typedef struct {
    char host[MAX_HOST_LEN];
    int port;
    char path[MAX_URL_LEN];
    char query[MAX_URL_LEN];
} http_url_t;

int http_parse_url(const char *url, http_url_t *parsed)
{
    memset(parsed, 0, sizeof(*parsed));
    
    /* Skip http:// or https:// */
    const char *p = url;
    int https = 0;
    
    if (strncmp(p, "https://", 8) == 0) {
        https = 1;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }
    
    /* Parse host */
    const char *host_start = p;
    while (*p && *p != '/' && *p != ':' && *p != '?') p++;
    
    size_t host_len = p - host_start;
    if (host_len >= MAX_HOST_LEN) return -1;
    
    memcpy(parsed->host, host_start, host_len);
    parsed->host[host_len] = '\0';
    
    /* Parse port */
    if (*p == ':') {
        p++;
        parsed->port = atoi(p);
        while (*p && *p != '/' && *p != '?') p++;
    } else {
        parsed->port = https ? HTTPS_PORT : HTTP_PORT;
    }
    
    /* Parse path */
    if (*p == '/') {
        const char *path_start = p;
        while (*p && *p != '?') p++;
        
        size_t path_len = p - path_start;
        if (path_len >= MAX_URL_LEN) return -1;
        
        memcpy(parsed->path, path_start, path_len);
        parsed->path[path_len] = '\0';
    } else {
        strcpy(parsed->path, "/");
    }
    
    /* Parse query */
    if (*p == '?') {
        p++;
        strncpy(parsed->query, p, MAX_URL_LEN - 1);
    }
    
    return 0;
}

int http_get(const char *url, const char *output_file)
{
    http_url_t parsed;
    
    if (http_parse_url(url, &parsed) < 0) {
        fprintf(stderr, "httpget: invalid URL: %s\n", url);
        return 1;
    }
    
    printf("httpget: connecting to %s:%d\n", parsed.host, parsed.port);
    
    /* TODO: Implement TCP socket connect */
    /* For now, just print what we would do */
    
    char request[2048];
    snprintf(request, sizeof(request),
        "GET %s%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: TNU-httpget/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        parsed.path, parsed.query, parsed.host);
    
    printf("httpget: request:\n%s\n", request);
    
    /* TODO: Send request and receive response */
    
    printf("httpget: downloading %s to %s\n", url, output_file);
    
    return 0;
}

int http_post(const char *url, const char *data, const char *content_type)
{
    http_url_t parsed;
    
    if (http_parse_url(url, &parsed) < 0) {
        fprintf(stderr, "httpget: invalid URL: %s\n", url);
        return 1;
    }
    
    size_t data_len = strlen(data);
    
    printf("httpget: POST to %s:%d%s\n", parsed.host, parsed.port, parsed.path);
    
    char request[4096];
    snprintf(request, sizeof(request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: TNU-httpget/1.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        parsed.path, parsed.host, content_type, data_len, data);
    
    printf("httpget: request:\n%s\n", request);
    
    return 0;
}

void print_usage(const char *prog)
{
    printf("Usage: %s [-o output] [-X POST] [-d data] URL\n", prog);
    printf("  -o FILE    Write output to FILE\n");
    printf("  -X METHOD  HTTP method (GET, POST)\n");
    printf("  -d DATA    POST data\n");
    printf("  -h         Show this help\n");
}

int main(int argc, char **argv)
{
    char *output_file = NULL;
    char *post_data = NULL;
    char *method = "GET";
    char *url = NULL;
    
    int opt;
    while ((opt = getopt(argc, argv, "o:X:d:h")) != -1) {
        switch (opt) {
        case 'o':
            output_file = optarg;
            break;
        case 'X':
            method = optarg;
            break;
        case 'd':
            post_data = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }
    
    if (optind >= argc) {
        fprintf(stderr, "httpget: missing URL\n");
        print_usage(argv[0]);
        return 1;
    }
    
    url = argv[optind];
    
    if (!output_file) {
        output_file = "download";
    }
    
    if (strcmp(method, "POST") == 0 || post_data) {
        return http_post(url, post_data ? post_data : "", "application/x-www-form-urlencoded");
    } else {
        return http_get(url, output_file);
    }
}
