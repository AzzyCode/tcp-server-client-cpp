# Simple TCP/IP Server and Client

This repository contains the implementation of a basic TCP/IP server and client using sockets that communicate over a network to perform operations on a simple key-value store.

## Features

- **TCP Server**: Handles multiple client connections using non-blocking sockets.
- **Key-Value Store**: Supports basic operations like SET, GET, and DELETE.
- **Custom Binary Protocol**: Uses a simple protocol for message passing between client and server.

## Requirements

- GCC compiler
- Linux or Unix-like OS

## Building the Code

Clone the repository and compile the server and client:

```bash
git clone https://github.com/yourusername/simple-tcpip-server-client.git
cd simple-tcpip-server-client
g++ server.cpp -o server
g++ client.cpp -o client
```
## Running the Server

To start the server, navigate to the directory where the server executable is located and run:

```bash
./server
```

The server will listen on port 1234 for incoming connections. Ensure the port is available and not blocked by any firewall settings.

## Running the Client

To interact with the server using the client, open a new terminal window and run the following commands depending on the action you want to perform:

### Set Value
```bash
./client set your_key your_value
```
Replace your_key and your_value with the key and value you wish to set.

### Get a value:
```bash
./client get your_key
```
Replace your_key with the key whose value you want to retrieve.

### Delete a key:
```bash
./client del your_key
```
Replace your_key with the key you wish to delete.

These commands allow you to interact directly with the key-value store implemented by the server.

## Usage Examples

### Setting Value:
```bash
./client set username alice
```

### Retrieving a value:
```bash
./client get username
```

### Deleting a value:
```bash
./client del username
```

