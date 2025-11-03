#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>

#define BUFFER_SIZE 4096

// Function prototypes
int validate_root_directory(const char *root_path);
int build_file_path(const char *root, const char *req_path, char *file_path, size_t max_len);
int send_file(int socket, const char *file_path, const char *req_path);
int is_directory(const char *path);
void normalize_path(char *path);
void send_404(int socket, const char *path);
void send_403(int socket, const char *path);
void send_400(int socket);
const char* get_mime_type(const char *path);
int sanitize_url(char *url);
int contains_path_traversal(const char *path);
int is_file_readable(const char *path);
void strip_query_and_fragment(char *url);
int validate_url_characters(const char *url);
void handle_client(int client_socket, const char *root_dir);
void setup_signal_handler(void);
void sigchld_handler(int sig);

int main(int argc, char *argv[]) {
    // Check command line arguments
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port> <root_directory>\n", argv[0]);
        fprintf(stderr, "Example: %s 8080 ./www\n", argv[0]);
        exit(1);
    }

    int port = atoi(argv[1]);
    const char *root_dir = argv[2];

    // Validate root directory
    if (!validate_root_directory(root_dir)) {
        fprintf(stderr, "Error: '%s' is not a valid directory\n", root_dir);
        exit(1);
    }
    printf("Serving files from: %s\n", root_dir);

    // Setup signal handler for child processes
    setup_signal_handler();

    // Socket setup
    int serverSocket;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t addr_size;

    // Create socket
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        perror("Socket creation failed");
        exit(1);
    }
    printf("Socket created successfully.\n");

    // Set socket option to reuse address
    int opt = 1;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(serverSocket);
        exit(1);
    }

    // Set up server address
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    // Bind socket
    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("Bind failed");
        close(serverSocket);
        exit(1);
    }
    printf("Bind successful on port %d.\n", port);

    // Listen for connections
    if (listen(serverSocket, 5) == 0)
        printf("Listening for connections...\n");
    else {
        perror("Listen failed");
        exit(1);
    }

    // Main server loop - Parent process accepts connections
    while (1) {
        // Accept client connection
        addr_size = sizeof(clientAddr);
        int newSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &addr_size);
        if (newSocket < 0) {
            if (errno == EINTR) {
                // Interrupted by signal, continue accepting
                continue;
            }
            perror("Accept failed");
            continue;
        }

        printf("\n[PARENT PID=%d] Client connected from %s:%d (socket=%d)\n",
               getpid(), inet_ntoa(clientAddr.sin_addr), 
               ntohs(clientAddr.sin_port), newSocket);

        // Fork a child process to handle the client
        pid_t pid = fork();

        if (pid < 0) {
            // Fork failed
            perror("Fork failed");
            close(newSocket);
            continue;
        }
        else if (pid == 0) {
            // ============ CHILD PROCESS ============
            printf("[CHILD PID=%d] Handling client request\n", getpid());
            
            // Child doesn't need the listening socket
            close(serverSocket);
            
            // Handle the client request
            handle_client(newSocket, root_dir);
            
            // Close client socket
            close(newSocket);
            printf("[CHILD PID=%d] Request handled, exiting\n", getpid());
            
            // Exit child process
            exit(0);
        }
        else {
            // ============ PARENT PROCESS ============
            printf("[PARENT PID=%d] Forked child process PID=%d\n", getpid(), pid);
            
            // Parent doesn't need the client socket
            close(newSocket);
            
            // Parent continues to accept more connections
            printf("[PARENT PID=%d] Ready to accept next connection\n", getpid());
        }
    }

    close(serverSocket);
    return 0;
}

// Setup signal handler for SIGCHLD
void setup_signal_handler(void) {
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction failed");
        exit(1);
    }
    printf("Signal handler for SIGCHLD installed.\n");
}

// Signal handler to reap zombie processes
void sigchld_handler(int sig) {
    // Save errno to restore it later (good practice in signal handlers)
    int saved_errno = errno;
    
    // Reap all terminated child processes
    pid_t pid;
    int status;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFEXITED(status)) {
            printf("[SIGNAL HANDLER] Child process PID=%d exited with status %d\n", 
                   pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("[SIGNAL HANDLER] Child process PID=%d killed by signal %d\n", 
                   pid, WTERMSIG(status));
        }
    }
    
    // Restore errno
    errno = saved_errno;
}

// Handle individual client request (runs in child process)
void handle_client(int client_socket, const char *root_dir) {
    // Receive HTTP request
    char req_buf[BUFFER_SIZE];
    ssize_t rlen = recv(client_socket, req_buf, sizeof(req_buf) - 1, 0);
    if (rlen < 0) {
        perror("recv failed");
        return;
    } else if (rlen == 0) {
        printf("[CHILD PID=%d] Client closed connection\n", getpid());
        return;
    }

    req_buf[rlen] = '\0';
    printf("[CHILD PID=%d] Received %zd bytes\n", getpid(), rlen);

    // Extract request line
    char request_line[1024];
    char *line_end = strstr(req_buf, "\r\n");
    if (!line_end) line_end = strstr(req_buf, "\n");
    
    if (line_end) {
        size_t line_len = line_end - req_buf;
        if (line_len >= sizeof(request_line)) 
            line_len = sizeof(request_line) - 1;
        strncpy(request_line, req_buf, line_len);
        request_line[line_len] = '\0';
    } else {
        printf("[CHILD PID=%d] No request line found.\n", getpid());
        send_400(client_socket);
        return;
    }

    // Parse request line
    char method[16], path[1024], version[16];
    int scanned = sscanf(request_line, "%15s %1023s %15s", method, path, version);
    
    if (scanned != 3) {
        send_400(client_socket);
        return;
    }

    printf("[CHILD PID=%d] Request: %s %s %s\n", getpid(), method, path, version);

    // Only handle GET requests
    if (strcmp(method, "GET") != 0) {
        const char *not_impl = "HTTP/1.1 501 Not Implemented\r\n"
                              "Content-Type: text/html\r\n"
                              "Content-Length: 70\r\n"
                              "Connection: close\r\n\r\n"
                              "<html><body><h1>501 Not Implemented</h1>"
                              "<p>Method not supported.</p></body></html>";
        send(client_socket, not_impl, strlen(not_impl), 0);
        printf("[CHILD PID=%d] Sent 501 Not Implemented (method %s)\n", getpid(), method);
        return;
    }

    // Sanitize URL
    int sanitize_result = sanitize_url(path);
    printf("[CHILD PID=%d] Sanitize result: %d\n", getpid(), sanitize_result);
    
    if (!sanitize_result) {
        printf("[CHILD PID=%d] URL failed sanitization: %s\n", getpid(), path);
        send_400(client_socket);
        return;
    }

    printf("[CHILD PID=%d] Sanitized path: %s\n", getpid(), path);

    // Build file path
    char file_path[2048];
    build_file_path(root_dir, path, file_path, sizeof(file_path));
    
    // Check if file is readable
    if (!is_file_readable(file_path)) {
        printf("[CHILD PID=%d] File not readable or not found: %s\n", getpid(), file_path);
        
        // Determine if it's 403 or 404
        struct stat st;
        if (stat(file_path, &st) == 0) {
            send_403(client_socket, path);
        } else {
            send_404(client_socket, path);
        }
        return;
    }

    // Send the file
    if (!send_file(client_socket, file_path, path)) {
        send_404(client_socket, path);
    }
}

// Validate that root directory exists and is accessible
int validate_root_directory(const char *root_path) {
    struct stat path_stat;
    if (stat(root_path, &path_stat) != 0) {
        return 0;
    }
    return S_ISDIR(path_stat.st_mode);
}

int is_directory(const char *path) {
    struct stat path_stat;
    if (stat(path, &path_stat) != 0) {
        return 0;
    }
    return S_ISDIR(path_stat.st_mode);
}

// Build full file path from root and request path
int build_file_path(const char *root, const char *req_path, char *file_path, size_t max_len) {
    size_t root_len = strlen(root);
    char root_clean[4096];
    strncpy(root_clean, root, sizeof(root_clean) - 1);
    root_clean[sizeof(root_clean) - 1] = '\0';
    
    if (root_len > 0 && root_clean[root_len - 1] == '/') {
        root_clean[root_len - 1] = '\0';
    }
    
    if (strcmp(req_path, "/") == 0) {
        snprintf(file_path, max_len, "%s/index.html", root_clean);
    } else {
        snprintf(file_path, max_len, "%s%s", root_clean, req_path);
        
        if (is_directory(file_path)) {
            size_t current_len = strlen(file_path);
            
            if (current_len > 0 && file_path[current_len - 1] == '/') {
                file_path[current_len - 1] = '\0';
                current_len--;
            }
            
            char temp_path[4096];
            strncpy(temp_path, file_path, sizeof(temp_path) - 1);
            temp_path[sizeof(temp_path) - 1] = '\0';
            
            snprintf(file_path, max_len, "%s/index.html", temp_path);
            printf("[PID=%d] Directory detected, serving: %s\n", getpid(), file_path);
        }
    }
    
    return 1;
}

// Get MIME type based on file extension
const char* get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
        return "text/html";
    if (strcmp(ext, ".css") == 0)
        return "text/css";
    if (strcmp(ext, ".js") == 0)
        return "application/javascript";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(ext, ".png") == 0)
        return "image/png";
    if (strcmp(ext, ".gif") == 0)
        return "image/gif";
    if (strcmp(ext, ".txt") == 0)
        return "text/plain";
    
    return "application/octet-stream";
}

void normalize_path(char *path) {
    if (!path || strlen(path) == 0) return;
    
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
    
    if (path[0] != '/') {
        memmove(path + 1, path, strlen(path) + 1);
        path[0] = '/';
    }
}

// Send file contents to client
int send_file(int socket, const char *file_path, const char *req_path) {
    FILE *file = fopen(file_path, "rb");
    if (!file) {
        printf("[PID=%d] File not found: %s\n", getpid(), file_path);
        return 0;
    }

    struct stat file_stat;
    if (stat(file_path, &file_stat) != 0) {
        fclose(file);
        return 0;
    }

    long file_size = file_stat.st_size;
    const char *mime_type = get_mime_type(file_path);

    char header[512];
    int hlen = snprintf(header, sizeof(header),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %ld\r\n"
                       "Connection: close\r\n\r\n",
                       mime_type, file_size);
    send(socket, header, hlen, 0);

    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    long total_sent = 0;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        send(socket, buffer, bytes_read, 0);
        total_sent += bytes_read;
    }

    fclose(file);
    printf("[PID=%d] Sent 200 OK: %s (%ld bytes, %s)\n", 
           getpid(), req_path, total_sent, mime_type);
    return 1;
}

// Strip query strings (?param=value) and fragments (#anchor)
void strip_query_and_fragment(char *url) {
    char *query = strchr(url, '?');
    if (query) {
        *query = '\0';
    }
    
    char *fragment = strchr(url, '#');
    if (fragment) {
        *fragment = '\0';
    }
}

// Validate that URL contains only safe characters
int validate_url_characters(const char *url) {
    for (const char *p = url; *p != '\0'; p++) {
        char c = *p;
        
        if (isalnum(c) || c == '/' || c == '.' || c == '-' || c == '_' || c == '%') {
            continue;
        }
        
        printf("[PID=%d] Invalid character in URL: '%c' (0x%02x)\n", 
               getpid(), c, (unsigned char)c);
        return 0;
    }
    return 1;
}

// Check for path traversal attempts (../)
int contains_path_traversal(const char *path) {
    printf("[PID=%d]   [Security] Checking path traversal for: '%s'\n", getpid(), path);
    
    if (strstr(path, "../") != NULL) {
        printf("[PID=%d]   [Security] BLOCKED: Found '../' in path\n", getpid());
        return 1;
    }
    
    if (strstr(path, "/..") != NULL) {
        printf("[PID=%d]   [Security] BLOCKED: Found '/..' in path\n", getpid());
        return 1;
    }
    
    if (strcmp(path, "..") == 0) {
        printf("[PID=%d]   [Security] BLOCKED: Path is '..'\n", getpid());
        return 1;
    }
    
    printf("[PID=%d]   [Security] Path traversal check: PASSED\n", getpid());
    return 0;
}

// Check if file exists and is readable
int is_file_readable(const char *path) {
    if (access(path, R_OK) == 0) {
        return 1;
    }
    return 0;
}

// Main URL sanitization function
int sanitize_url(char *url) {
    printf("[PID=%d] Original URL: %s\n", getpid(), url);
    
    strip_query_and_fragment(url);
    printf("[PID=%d] After stripping query/fragment: %s\n", getpid(), url);
    
    if (contains_path_traversal(url)) {
        return 0;
    }
    
    if (!validate_url_characters(url)) {
        return 0;
    }
    
    normalize_path(url);
    
    return 1;
}

// Send 404 Not Found response
void send_404(int socket, const char *path) {
    const char *body = "<html><body><h1>404 Not Found</h1>"
                      "<p>The requested resource was not found.</p></body></html>";
    char response[1024];
    int len = snprintf(response, sizeof(response),
                      "HTTP/1.1 404 Not Found\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n\r\n%s",
                      strlen(body), body);
    send(socket, response, len, 0);
    printf("[PID=%d] Sent 404 Not Found: %s\n", getpid(), path);
}

// Send 403 Forbidden response
void send_403(int socket, const char *path) {
    const char *body = "<html><body><h1>403 Forbidden</h1>"
                      "<p>You don't have permission to access this resource.</p></body></html>";
    char response[1024];
    int len = snprintf(response, sizeof(response),
                      "HTTP/1.1 403 Forbidden\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n\r\n%s",
                      strlen(body), body);
    send(socket, response, len, 0);
    printf("[PID=%d] Sent 403 Forbidden: %s\n", getpid(), path);
}

// Send 400 Bad Request response
void send_400(int socket) {
    const char *body = "<html><body><h1>400 Bad Request</h1>"
                      "<p>Your request could not be understood.</p></body></html>";
    char response[1024];
    int len = snprintf(response, sizeof(response),
                      "HTTP/1.1 400 Bad Request\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n\r\n%s",
                      strlen(body), body);
    send(socket, response, len, 0);
    printf("[PID=%d] Sent 400 Bad Request\n", getpid());
}