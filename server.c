#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>

#define PORT 8080
#define BUFFER_SIZE 8192
#define MAX_CLIENTS 10
#define KEEP_ALIVE_TIMEOUT 10
#define CACHE_SIZE 10

// Global variables for graceful shutdown
int server_fd = -1;
int clients[MAX_CLIENTS] = {0};

// ===================== MIME TYPE DETECTION =====================
typedef struct {
    const char *ext;
    const char *mime;
} MimeType;

MimeType mime_types[] = {
    {".html", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".ico", "image/x-icon"},
    {".json", "application/json"},
    {".txt", "text/plain"},
    {NULL, "application/octet-stream"}
};

const char* get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    for (int i = 0; mime_types[i].ext; i++) {
        if (strcmp(ext, mime_types[i].ext) == 0)
            return mime_types[i].mime;
    }
    return "application/octet-stream";
}

// ===================== URL SECURITY & SANITIZATION =====================

// Strip query strings (?param=value) and fragments (#anchor)
void strip_query_and_fragment(char *url) {
    char *query = strchr(url, '?');
    if (query) {
        *query = '\0';  // Terminate at '?'
    }
    
    char *fragment = strchr(url, '#');
    if (fragment) {
        *fragment = '\0';  // Terminate at '#'
    }
}

// Validate that URL contains only safe characters
int validate_url_characters(const char *url) {
    for (const char *p = url; *p != '\0'; p++) {
        char c = *p;
        
        // Allow: alphanumeric, slash, dot, hyphen, underscore, percent (for encoding)
        if (isalnum(c) || c == '/' || c == '.' || c == '-' || c == '_' || c == '%') {
            continue;
        }
        
        // Reject any other character
        printf("Invalid character in URL: '%c' (0x%02x)\n", c, (unsigned char)c);
        return 0;
    }
    return 1;
}

// Check for path traversal attempts (../)
int contains_path_traversal(const char *path) {
    printf("  [Security] Checking path traversal for: '%s'\n", path);
    
    // Check for literal "../" or "/.."
    if (strstr(path, "../") != NULL) {
        printf("  [Security] BLOCKED: Found '../' in path\n");
        return 1;
    }
    
    if (strstr(path, "/..") != NULL) {
        printf("  [Security] BLOCKED: Found '/..' in path\n");
        return 1;
    }
    
    // Check if path equals ".."
    if (strcmp(path, "..") == 0) {
        printf("  [Security] BLOCKED: Path is '..'\n");
        return 1;
    }
    
    printf("  [Security] Path traversal check: PASSED\n");
    return 0;
}

// Normalize path (remove duplicate slashes)
void normalize_path(char *path) {
    char *src = path;
    char *dst = path;
    int last_was_slash = 0;
    
    while (*src) {
        if (*src == '/') {
            if (!last_was_slash) {
                *dst++ = *src;
                last_was_slash = 1;
            }
        } else {
            *dst++ = *src;
            last_was_slash = 0;
        }
        src++;
    }
    *dst = '\0';
}

// Check if file exists and is readable
int is_file_readable(const char *path) {
    // Use access() to check if file is readable
    if (access(path, R_OK) == 0) {
        return 1;  // File is readable
    }
    return 0;  // File doesn't exist or isn't readable
}

// Main URL sanitization function
int sanitize_url(char *url) {
    printf("[SANITIZE] Original URL: %s\n", url);
    
    // Step 1: Strip query strings and fragments
    strip_query_and_fragment(url);
    printf("[SANITIZE] After stripping query/fragment: %s\n", url);
    
    // Step 2: Check for path traversal
    if (contains_path_traversal(url)) {
        printf("[SANITIZE] URL REJECTED: path traversal detected\n");
        return 0;  // Reject
    }
    
    // Step 3: Validate characters
    if (!validate_url_characters(url)) {
        printf("[SANITIZE] URL REJECTED: invalid characters\n");
        return 0;  // Reject
    }
    
    // Step 4: Normalize path (remove duplicate slashes)
    normalize_path(url);
    printf("[SANITIZE] Final sanitized URL: %s\n", url);
    
    return 1;  // URL is safe
}

// ===================== CACHING SYSTEM =====================
typedef struct {
    char path[256];
    char *data;
    size_t size;
    time_t last_modified;
} CacheEntry;

CacheEntry cache[CACHE_SIZE];
int cache_count = 0;

char* get_cached_file(const char *path, size_t *size) {
    printf("[CACHE] Attempting to read file: %s\n", path);
    
    struct stat st;
    if (stat(path, &st) == -1) {
        printf("[CACHE] File not found or stat failed: %s (errno: %d - %s)\n", path, errno, strerror(errno));
        return NULL;
    }
    
    printf("[CACHE] File found, size: %ld bytes\n", st.st_size);

    for (int i = 0; i < cache_count; i++) {
        if (strcmp(cache[i].path, path) == 0) {
            if (cache[i].last_modified == st.st_mtime) {
                printf("[CACHE] Serving from cache: %s\n", path);
                *size = cache[i].size;
                return cache[i].data;
            } else {
                printf("[CACHE] Cache expired, reloading: %s\n", path);
                free(cache[i].data);
                cache[i] = cache[--cache_count];
                break;
            }
        }
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("[CACHE] Failed to open file: %s (errno: %d - %s)\n", path, errno, strerror(errno));
        return NULL;
    }

    char *data = malloc(st.st_size);
    size_t bytes_read = fread(data, 1, st.st_size, fp);
    fclose(fp);
    
    printf("[CACHE] Read %zu bytes from file: %s\n", bytes_read, path);

    if (cache_count < CACHE_SIZE) {
        strcpy(cache[cache_count].path, path);
        cache[cache_count].data = data;
        cache[cache_count].size = st.st_size;
        cache[cache_count].last_modified = st.st_mtime;
        cache_count++;
        printf("[CACHE] Added to cache (total cached: %d)\n", cache_count);
    }

    *size = st.st_size;
    return data;
}

// ===================== ROUTING SYSTEM =====================
typedef void (*RouteHandler)(int, const char*, const char*);
typedef struct {
    char path[64];
    RouteHandler handler;
} Route;

void handle_api_route(int client, const char *method, const char *body) {
    const char *response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"message\":\"API route active\"}";
    send(client, response, strlen(response), 0);
}

void handle_contact_route(int client, const char *method, const char *body) {
    char response[256];
    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nReceived: %s", body);
    send(client, response, strlen(response), 0);
}

Route routes[] = {
    {"/api", handle_api_route},
    {"/contact", handle_contact_route}
};
int route_count = 2;

Route* find_route(const char *path) {
    for (int i = 0; i < route_count; i++) {
        if (strcmp(path, routes[i].path) == 0)
            return &routes[i];
    }
    return NULL;
}

// ===================== HTTP POST PARSER =====================
void parse_post_data(const char *buffer, char *body, size_t size) {
    const char *data_start = strstr(buffer, "\r\n\r\n");
    if (data_start) strncpy(body, data_start + 4, size - 1);
}

// ===================== RESPONSE SENDER =====================
void send_file_response(int client, const char *path, int keep_alive) {
    printf("[RESPONSE] Preparing to send file: %s\n", path);
    
    size_t size;
    char *data = get_cached_file(path, &size);
    if (!data) {
        printf("[RESPONSE] Sending 404 Not Found for: %s\n", path);
        const char *not_found = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        send(client, not_found, strlen(not_found), 0);
        return;
    }

    const char *mime = get_mime_type(path);
    printf("[RESPONSE] MIME type: %s, Size: %zu bytes\n", mime, size);
    
    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n",
             mime, size, keep_alive ? "keep-alive" : "close");
    
    int header_sent = send(client, header, strlen(header), 0);
    int data_sent = send(client, data, size, 0);
    
    printf("[RESPONSE] Sent header (%d bytes) and data (%d bytes) for: %s\n", header_sent, data_sent, path);
}

// ===================== GRACEFUL SHUTDOWN =====================
// SIGINT handler for graceful shutdown
void sigint_handler(int sig) {
    printf("\n[SERVER] Caught SIGINT (Ctrl+C). Shutting down gracefully...\n");
    
    // Close all client connections
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] > 0) {
            close(clients[i]);
            printf("[SERVER] Closed client connection: %d\n", clients[i]);
        }
    }
    
    // Close server socket
    if (server_fd != -1) {
        close(server_fd);
        printf("[SERVER] Server socket closed.\n");
    }
    
    // Free cached data
    for (int i = 0; i < cache_count; i++) {
        free(cache[i].data);
    }
    printf("[SERVER] Cache cleared.\n");
    
    printf("[SERVER] Shutdown complete.\n");
    exit(0);
}

// ===================== MAIN SERVER LOOP =====================
int main() {
    int client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // Register SIGINT handler
    signal(SIGINT, sigint_handler);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        exit(1);
    }
    printf("[INIT] Socket created: fd=%d\n", server_fd);
    
    // Set SO_REUSEADDR to avoid "Address already in use" errors
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Setsockopt failed");
        exit(1);
    }
    printf("[INIT] SO_REUSEADDR set\n");
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(1);
    }
    printf("[INIT] Bound to port %d\n", PORT);
    
    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        exit(1);
    }
    printf("[INIT] Listening for connections...\n");

    printf("Server running on http://localhost:%d\n", PORT);
    printf("Press Ctrl+C to shutdown gracefully.\n");

    fd_set readfds;
    int max_fd = server_fd;

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        max_fd = server_fd;
        
        int active_clients = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] > 0) {
                FD_SET(clients[i], &readfds);
                active_clients++;
                if (clients[i] > max_fd)
                    max_fd = clients[i];
            }
        }

        printf("[SELECT] Waiting for activity (server_fd=%d, max_fd=%d, active_clients=%d)...\n", 
               server_fd, max_fd, active_clients);
        
        int activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        
        printf("[SELECT] Activity detected! Return value: %d\n", activity);
        
        if (activity < 0) {
            printf("[SELECT] Error in select: %s\n", strerror(errno));
            continue;
        }
        if (FD_ISSET(server_fd, &readfds)) {
            client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            printf("\n[SERVER] New connection accepted: socket %d from %s:%d\n", 
                   client_fd, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i] == 0) {
                    clients[i] = client_fd;
                    printf("[SERVER] Client assigned to slot %d\n", i);
                    break;
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int sd = clients[i];
            if (FD_ISSET(sd, &readfds)) {
                char buffer[BUFFER_SIZE], body[BUFFER_SIZE] = {0};
                int bytes = recv(sd, buffer, sizeof(buffer) - 1, 0);
                
                printf("\n[CONNECTION] Activity on client socket %d\n", sd);
                
                if (bytes <= 0) {
                    printf("[CONNECTION] Client %d disconnected (bytes: %d)\n", sd, bytes);
                    close(sd);
                    clients[i] = 0;
                } else {
                    buffer[bytes] = '\0';
                    printf("[REQUEST] Received %d bytes from client %d\n", bytes, sd);
                    printf("[REQUEST] First line: %.100s\n", buffer);
                    
                    int keep_alive = strstr(buffer, "Connection: keep-alive") != NULL;
                    char method[8], path[256];
                    sscanf(buffer, "%s %s", method, path);
                    
                    printf("[REQUEST] Method: %s, Path: %s, Keep-Alive: %s\n", 
                           method, path, keep_alive ? "YES" : "NO");

                    // Sanitize the URL before processing
                    if (!sanitize_url(path)) {
                        printf("[REQUEST] URL sanitization FAILED - sending 403\n");
                        const char *forbidden = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\nInvalid URL";
                        send(sd, forbidden, strlen(forbidden), 0);
                        close(sd);
                        clients[i] = 0;
                        continue;
                    }

                    Route *route = find_route(path);
                    if (route) {
                        printf("[ROUTE] Found route handler for: %s\n", path);
                        parse_post_data(buffer, body, sizeof(body));
                        route->handler(sd, method, body);
                    } else {
                        char full_path[512];
                        
                        // Handle root path
                        if (strcmp(path, "/") == 0) {
                            snprintf(full_path, sizeof(full_path), "./www/index.html");
                        }
                        // Handle directory paths (ending with /)
                        else if (path[strlen(path) - 1] == '/') {
                            snprintf(full_path, sizeof(full_path), "./www%sindex.html", path);
                        }
                        // Handle file paths
                        else {
                            snprintf(full_path, sizeof(full_path), "./www%s", path);
                        }
                        
                        printf("[FILE] Constructed file path: %s\n", full_path);
                        
                        // Additional security check for file readability
                        if (!is_file_readable(full_path)) {
                            printf("[FILE] File not readable: %s\n", full_path);
                            const char *not_found = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                            send(sd, not_found, strlen(not_found), 0);
                            if (!keep_alive) {
                                close(sd);
                                clients[i] = 0;
                            }
                            continue;
                        }
                        
                        send_file_response(sd, full_path, keep_alive);
                    }

                    if (!keep_alive) {
                        printf("[CONNECTION] Closing connection (no keep-alive)\n");
                        close(sd);
                        clients[i] = 0;
                    }
                }
            }
        }
    }

    close(server_fd);
    return 0;
}