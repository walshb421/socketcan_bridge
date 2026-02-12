#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BUFFER_SIZE 1024

int main(int argc, char *argv[]) {
    int client_fd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE] = {0};
    char *server_ip = "127.0.0.1";  // localhost
    
    // Allow specifying server IP as command line argument
    if (argc > 1) {
        server_ip = argv[1];
    }
    
    // Create socket
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Configure server address structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    
    // Convert IP address from text to binary form
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address or address not supported");
        close(client_fd);
        exit(EXIT_FAILURE);
    }
    
    // Connect to server
    if (connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("Connection failed");
        close(client_fd);
        exit(EXIT_FAILURE);
    }
    
    printf("Connected to server at %s:%d\n", server_ip, PORT);
    printf("Type messages (Ctrl+D to quit):\n");
    
    // Communication loop
    while (1) {
        printf("> ");
        fflush(stdout);
        
        // Read user input
        if (fgets(buffer, BUFFER_SIZE, stdin) == NULL) {
            printf("\nClosing connection...\n");
            break;
        }
        
        // Send message to server
        if (write(client_fd, buffer, strlen(buffer)) == -1) {
            perror("Write failed");
            break;
        }
        
        // Clear buffer and read server response
        memset(buffer, 0, BUFFER_SIZE);
        ssize_t bytes_read = read(client_fd, buffer, BUFFER_SIZE - 1);
        
        if (bytes_read <= 0) {
            printf("Server disconnected\n");
            break;
        }
        
        printf("Echo: %s", buffer);
    }
    
    // Cleanup
    close(client_fd);
    
    return 0;
}
