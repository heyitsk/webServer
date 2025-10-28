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

    // 6️⃣ Send a message to the client
    send(newSocket, buffer, strlen(buffer), 0);
    printf("Message sent to client.\n");

    // 7️⃣ Close sockets
    close(newSocket);
    close(serverSocket);
    printf("Server closed.\n");

    return 0;
}

