#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>         // for close()
#include <arpa/inet.h>      // for sockaddr_in, inet_ntoa()

int main() {
    int serverSocket, newSocket;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t addr_size;
    char buffer[1024] = "Hello from server!";
    char request_line[1024];

    // 1️⃣ Create a TCP socket
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        perror("Socket creation failed");
        exit(1);
    }
    printf("Socket created successfully.\n");

    // 2️⃣ Set up the server address structure
    serverAddr.sin_family = AF_INET;             // IPv4
    serverAddr.sin_port = htons(8080);           // Port 8080 (convert to network byte order)
    serverAddr.sin_addr.s_addr = INADDR_ANY;     // Accept connections from any IP

    // 3️⃣ Bind the socket to the given IP and port
    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("Bind failed");
        close(serverSocket);
        exit(1);
    }
    printf("Bind successful.\n");

    // 4️⃣ Listen for incoming connections (max queue = 5)
    if (listen(serverSocket, 5) == 0)
        printf("Listening for connections...\n");
    else {
        perror("Listen failed");
        exit(1);
    }

    // 5️⃣ Accept a client connection
    addr_size = sizeof(clientAddr);
    newSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &addr_size);
    if (newSocket < 0) {
        perror("Accept failed");
        exit(1);
    }
    printf("Client connected from %s:%d\n", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));

    // after accept(...)
    char req_buf[4096];
    ssize_t rlen = recv(newSocket, req_buf, sizeof(req_buf) - 1, 0);
    if (rlen < 0) {
        perror("recv failed");
        // handle error
    } else if (rlen == 0) {
        // client closed connection
        printf("Client closed connection\n");
        req_buf[0] = '\0';
    } else {
        // safe to null-terminate
        req_buf[rlen] = '\0';
        printf("Received %zd bytes:\n%s\n", rlen, req_buf);
        // proceed to parse
    }



    // assume req_buf is populated and null-terminated
    char *line_end = strstr(req_buf, "\r\n");
    if (!line_end) line_end = strstr(req_buf, "\n"); // fallback
    if (line_end) {
        size_t line_len = line_end - req_buf;
        
        if (line_len >= sizeof(request_line)) line_len = sizeof(request_line)-1;
        strncpy(request_line, req_buf, line_len);
        request_line[line_len] = '\0';
        printf("Request line: \"%s\"\n", request_line);
    } else {
        printf("No request line found.\n");
    }

    char method[16], path[1024], version[16];
    int scanned = sscanf(request_line, "%15s %1023s %15s", method, path, version); //this %15s limits input size to avoid overflow. This just divides request into 3 tokens 
    if (scanned != 3) {
        // malformed request
        const char *bad = "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Length: 11\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "Bad Request";
        send(newSocket, bad, strlen(bad), 0);
        printf("Sent 400 Bad Request\n");
        close(newSocket);
        close(serverSocket);
        return 0;
    }
    printf("Parsed -> method: %s, path: %s, version: %s\n", method, path, version);



    if (strcmp(method, "GET") != 0) {
    const char *not_impl = "HTTP/1.1 501 Not Implemented\r\n"
                           "Content-Length: 16\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "Not Implemented";
    send(newSocket, not_impl, strlen(not_impl), 0);
    printf("Sent 501 Not Implemented (method %s)\n", method);
    } else {
        // build a simple HTML body (you can improve later)
        const char *body = "<html><body><h1>Hello from C server</h1><p>Path: /</p></body></html>";
        char header[512];
        int hlen = snprintf(header, sizeof(header),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/html; charset=utf-8\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n"
                            "\r\n", strlen(body));
        // send header then body
        send(newSocket, header, hlen, 0);
        send(newSocket, body, strlen(body), 0);
        //sending the header and body spearately to avoid one large buffer and matches real world web server 
        printf("Sent 200 OK for %s\n", path);
    }



    // 7️⃣ Close sockets
    close(newSocket);
    close(serverSocket);
    printf("Server closed.\n");

    return 0;
}

// GET /index.html HTTP/1.1




