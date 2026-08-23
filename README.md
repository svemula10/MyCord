# MyCord - Concurrent TCP Chat Client & Terminal UI

MyCord is a multithreaded terminal-based chat client written in C for Linux/Unix systems. It communicates with a TCP IPv4 chat server using a custom binary message protocol and supports real-time messaging, server notifications, graceful shutdown, command-line configuration, and an interactive terminal user interface.

## 🌟 Key Features

- **TCP IPv4 Networking**: Establishes persistent socket connections to a MyCord server using configurable IP addresses, domains, and ports.
- **Custom Binary Protocol**: Implements the MyCord 1,064-byte message protocol for structured client-server communication.
- **Multithreaded Communication**: Uses POSIX pthread to receive server messages concurrently while the main thread handles user input.
- **Real-Time Chat**: Supports sending and receiving messages between multiple connected clients.
- **Message Validation**: Validates outgoing messages for length, printable ASCII characters, and invalid input before transmission.
- **Mention Notifications**: Detects @username mentions and highlights them with ANSI colors and terminal alerts.
- **Graceful Shutdown**: Handles SIGINT, SIGTERM, and EOF to cleanly terminate connections.
- **DNS Resolution**: Supports connecting to servers through domain names in addition to direct IPv4 addresses.
- **Interactive TUI**: Includes an optional terminal user interface with scrolling message history, persistent input, and keyboard-based navigation.

## 🛠️ Tech Stack

- **Language**: C
- **Platform**: Linux / Unix
- **Networking**: TCP IPv4, POSIX Sockets
- **Concurrency**: POSIX pthread
- **Signal Handling**: sigaction, SIGINT, SIGTERM
- **DNS**: gethostbyname
- **Terminal Interface**: ANSI escape sequences
- **I/O**: read, write, getline, printf, fprintf
- **Compilation**: GCC with pthread support

## 🚀 Getting Started Locally

### 1: Prerequisites

Make sure you have a Linux/Unix environment with GCC and Python 3 installed

Required tools:
- GCC (GNU Compiler Collection)
- Python 3
- POSIX-compatible operating system
- POSIX pthread support

### 2: Clone the Repository (if you haven't downloaded from github)
```
git clone https://github.com/svemula10/MyCord.git
cd MyCord
```

### 3: Compile the Client
Compile the client with pthread support:

```
gcc client.c -o client -pthread
```

### 4: Start the Local Server

Open a separate terminal and run:
```
python3 server.py
```
The server will display the port it is listening on.

### 5: Start the Client

In another terminal, run:
```
./client --port 1234
```
Replace 1234 with the port displayed by the server.

You can also connect using a domain

### 6: Run TUI Mode

To launch the interactive terminal interface:
```
./client --tui
```
The TUI provides scrollable message history, persistent input at the bottom of the terminal, and real-time message updates.
