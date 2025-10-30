#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>

#define BUFFER_SIZE 4096

// Function prototypes
int validate_root_directory(const char *root_path);
void build_file_path(const char *root, const char *req_path, char *file_path, size_t max_len);
int send_file(int socket, const char *file_path, const char *req_path);
void send_404(int socket, const char *path);
const char* get_mime_type(const char *path);

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

    // Socket setup
    int serverSocket, newSocket;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t addr_size;

    // Create socket
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        perror("Socket creation failed");
        exit(1);
    }
    printf("Socket created successfully.\n");

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

    // Main server loop
    while (1) {
        // Accept client connection
        addr_size = sizeof(clientAddr);
        newSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &addr_size);
        if (newSocket < 0) {
            perror("Accept failed");
            continue;
        }
        printf("\n=== Client connected from %s:%d ===\n",
               inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));

        // Receive HTTP request
        char req_buf[BUFFER_SIZE];
        ssize_t rlen = recv(newSocket, req_buf, sizeof(req_buf) - 1, 0);
        if (rlen < 0) {
            perror("recv failed");
            close(newSocket);
            continue;
        } else if (rlen == 0) {
            printf("Client closed connection\n");
            close(newSocket);
            continue;
        }

        req_buf[rlen] = '\0';
        printf("Received %zd bytes\n", rlen);

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
            printf("No request line found.\n");
            close(newSocket);
            continue;
        }

        // Parse request line
        char method[16], path[1024], version[16];
        int scanned = sscanf(request_line, "%15s %1023s %15s", method, path, version);
        
        if (scanned != 3) {
            const char *bad = "HTTP/1.1 400 Bad Request\r\n"
                            "Content-Length: 11\r\n"
                            "Connection: close\r\n\r\n"
                            "Bad Request";
            send(newSocket, bad, strlen(bad), 0);
            printf("Sent 400 Bad Request\n");
            close(newSocket);
            continue;
        }

        printf("Request: %s %s %s\n", method, path, version);

        // Only handle GET requests
        if (strcmp(method, "GET") != 0) {
            const char *not_impl = "HTTP/1.1 501 Not Implemented\r\n"
                                  "Content-Length: 16\r\n"
                                  "Connection: close\r\n\r\n"
                                  "Not Implemented";
            send(newSocket, not_impl, strlen(not_impl), 0);
            printf("Sent 501 Not Implemented (method %s)\n", method);
            close(newSocket);
            continue;
        }

        // Build file path and send file
        char file_path[2048];
        build_file_path(root_dir, path, file_path, sizeof(file_path));
        
        if (!send_file(newSocket, file_path, path)) {
            send_404(newSocket, path);
        }

        close(newSocket);
        printf("=== Connection closed ===\n");
    }

    close(serverSocket);
    return 0;
}

// Validate that root directory exists and is accessible
int validate_root_directory(const char *root_path) {
    struct stat path_stat;
    if (stat(root_path, &path_stat) != 0) {
        return 0; // stat() failed
    }
    return S_ISDIR(path_stat.st_mode); // Check if it's a directory
}

// Build full file path from root and request path
void build_file_path(const char *root, const char *req_path, char *file_path, size_t max_len) {
    // Start with root directory
    snprintf(file_path, max_len, "%s", root);
    
    // If path is "/", serve index.html
    if (strcmp(req_path, "/") == 0) {
        snprintf(file_path, max_len, "%s/index.html", root);
    } else {
        // Append requested path
        snprintf(file_path, max_len, "%s%s", root, req_path);
    }
    
    printf("Mapped path: '%s' -> '%s'\n", req_path, file_path);
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

// Send file contents to client
int send_file(int socket, const char *file_path, const char *req_path) {
    // Try to open file
    FILE *file = fopen(file_path, "rb");
    if (!file) {
        printf("File not found: %s\n", file_path);
        return 0;
    }

    // Get file size
    struct stat file_stat;
    if (stat(file_path, &file_stat) != 0) {
        fclose(file);
        return 0;
    }

    long file_size = file_stat.st_size;
    const char *mime_type = get_mime_type(file_path);

    // Send HTTP response header
    char header[512];
    int hlen = snprintf(header, sizeof(header),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %ld\r\n"
                       "Connection: close\r\n\r\n",
                       mime_type, file_size);
    send(socket, header, hlen, 0);

    // Send file contents in chunks
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    long total_sent = 0;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        send(socket, buffer, bytes_read, 0);
        total_sent += bytes_read;
    }

    fclose(file);
    printf("Sent 200 OK: %s (%ld bytes, %s)\n", req_path, total_sent, mime_type);
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
    printf("Sent 404 Not Found: %s\n", path);
}