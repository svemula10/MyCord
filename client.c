#define _POSIX_C_SOURCE 200809L // Enable POSIX features for getaddrinfo/getline
#include <stdbool.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <stdint.h>
#include <termios.h>
#include <sys/ioctl.h>

//Constants
#define PORT_DEFAULT "8080"
#define USERNAME_SIZE 32
#define MESSAGE_SIZE 1024
#define HISTORY_SIZE 100

typedef enum { 
    LOGIN = 0,
    LOGOUT = 1,
    MESSAGE_SEND = 2,
    MESSAGE_RECV = 10,
    DISCONNECT = 12,
    SYSTEM = 13
} MessageType;

typedef struct __attribute__((packed)){
    uint32_t type;
    uint32_t timestamp;
    char username[USERNAME_SIZE];
    char message[MESSAGE_SIZE];
} mycord_msg_t;

struct config_t {
    char *port;
    char *ip;
    char *domain;
    bool quiet;
    bool tui;
} config;

static char* COLOR_RED = "\033[31m";
static char* COLOR_GRAY = "\033[90m";
static char* COLOR_RESET = "\033[0m";


int sockfd = -1;
char my_username[USERNAME_SIZE];
volatile sig_atomic_t running = 1;
pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

// TUI State
char input_buffer[MESSAGE_SIZE];
int input_len = 0;
char *history[HISTORY_SIZE];
int history_count = 0;
struct termios orig_termios;
int scroll_offset = 0; // 0 = at bottom (newest messages)


void print_error(const char *msg){
    if (errno != 0) {
        fprintf(stderr, "Error: %s (%s)\n", msg, strerror(errno));
    } else {
        fprintf(stderr, "Error: %s\n", msg);
    }
}

// Restore terminal to normal mode (Canonical mode)
void disable_raw_mode() {
    if (config.tui) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        printf("\033[?1049l"); // Disable alternate screen buffer
        printf("\033[?25h");   // Show cursor
    }
}

// Enable raw mode (Capture keystrokes directly, no echo)
void enable_raw_mode() {
    if (config.tui) {
        tcgetattr(STDIN_FILENO, &orig_termios);
        atexit(disable_raw_mode);
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON); // Disable echo and canonical mode
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        
        printf("\033[?1049h"); // Enable alternate screen buffer
        printf("\033[H\033[2J"); // Clear screen
    }
}

// Send a logout message (Best effort)
void send_logout() {
    if (sockfd != -1) {
        mycord_msg_t msg = {0};
        msg.type = htonl(LOGOUT);
        write(sockfd, &msg, sizeof(msg));
    }
}

void handle_signal(int sig) {
    send_logout();
    if (sockfd != -1) close(sockfd);
    if (config.tui) disable_raw_mode();
    exit(0);
}

void tui_redraw() {
    if (!config.tui) return;

    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int height = ws.ws_row;
    int width = ws.ws_col;

    pthread_mutex_lock(&print_lock);

    // Clear Screen & Reset Cursor
    printf("\033[2J\033[H");

    // Draw Header
    printf("\033[44m\033[1;37m MyCord TUI - Connected as %s \033[0m\r\n", my_username);

    //  Draw History
    // We reserve two lines at the bottom (one for separator line, one for input bar)
    int visible_lines = height - 2;
    int start_index = history_count - visible_lines - scroll_offset;
    if (start_index < 0) start_index = 0;

    // Indicator if scrolling happened
    if (scroll_offset > 0) printf("\033[1;33m--- SCROLLING HISTORY (%d) ---\033[0m\r\n", scroll_offset);
    else printf("\r\n");

    for (int i = start_index; i < history_count - scroll_offset && i < history_count; i++) {
        if (i >= HISTORY_SIZE) break;
        if (history[i]) printf("%s\r\n", history[i]);
    }

    // Draw Input Bar at Bottom
    printf("\033[%d;1H", height - 1); // Move cursor to second to last line
    for(int i=0; i<width; i++) printf("-"); // Draw separator

    printf("\033[%d;1H", height); // Move cursor to last line
    printf("\033[2K"); // Clear line content
    printf("> %s", input_buffer); // Draw input buffer

    fflush(stdout);
    pthread_mutex_unlock(&print_lock);
}

void add_to_history(const char *formatted_msg) {
    if (!config.tui) {
        printf("%s\n", formatted_msg);
        return;
    }

    pthread_mutex_lock(&print_lock);

    // Shift history array if full
    if (history_count >= HISTORY_SIZE) {
        free(history[0]);
        for (int i = 0; i < HISTORY_SIZE - 1; i++) {
            history[i] = history[i+1];
        }
        history_count = HISTORY_SIZE - 1;
    }

    history[history_count] = strdup(formatted_msg);
    history_count++;

    pthread_mutex_unlock(&print_lock);

    // Only auto-scroll if user is currently at the bottom
    if (scroll_offset == 0) tui_redraw();
}



/* --- Networking Helpers --- */

// Helper: Ensure exactly 'count' bytes are read (handles partial TCP reads)
ssize_t perform_full_read(int fd, void *buf, size_t count) {
    size_t total_read = 0;
    char *ptr = (char *)buf;
    while (total_read < count) {
        ssize_t n = read(fd, ptr + total_read, count - total_read);
        if (n <= 0) return -1;
        total_read += n;
    }
    return 0;
}

// Helper: Establish connection using modern getaddrinfo
int setup_connection() {
    struct addrinfo hints, *res;
    int status;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_STREAM; // TCP

    const char *target = config.domain ? config.domain : config.ip;
    
    // Resolve address (Handles both IP strings and Domain names)
    if ((status = getaddrinfo(target, config.port, &hints, &res)) != 0) {
        fprintf(stderr, "Error: getaddrinfo: %s\n", gai_strerror(status));
        return -1;
    }

    // Try to connect to the first valid address
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        print_error("Socket creation failed");
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        print_error("Connection failed");
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return fd;
}

void get_username() {
    char *env = getenv("USER");
    if (env && strlen(env) > 0) {
        strncpy(my_username, env, USERNAME_SIZE - 1);
    } else {
        FILE *fp = popen("whoami", "r");
        if (!fp || !fgets(my_username, sizeof(my_username), fp)) {
            print_error("Could not determine username");
            exit(1);
        }
        pclose(fp);
        my_username[strcspn(my_username, "\n")] = 0;
    }
    
    // Validation
    if (strlen(my_username) == 0) {
        fprintf(stderr, "Error: Empty username\n");
        exit(1);
    }
    for (int i = 0; my_username[i]; i++) {
        if (!isalnum(my_username[i])) {
            fprintf(stderr, "Error: Invalid username characters\n");
            exit(1);
        }
    }
}

int process_args(int argc, char *argv[]) {
    
    // Defaults
    config.port = PORT_DEFAULT;
    config.ip = "127.0.0.1";
    config.domain = NULL;
    config.quiet = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: ./client [-h] [--port PORT] [--ip IP] [--domain DOMAIN] [--quiet]\n");
            exit(0);
        } else if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) config.port = argv[++i];
            else return -1;
        } else if (strcmp(argv[i], "--ip") == 0) {
            if (i + 1 < argc) config.ip = argv[++i];
            else return -1;
        } else if (strcmp(argv[i], "--domain") == 0) {
            if (i + 1 < argc) config.domain = argv[++i];
            else return -1;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            config.quiet = true;
        } else if (strcmp(argv[i], "--tui") == 0) config.tui = true; // New Flag
	else {
            fprintf(stderr, "Error: Unknown argument '%s'\n", argv[i]);
            return -1;
        }
    }

    if (config.domain && strcmp(config.ip, "127.0.0.1") != 0) {
        fprintf(stderr, "Error: Cannot specify both --ip and --domain\n");
        return -1;
    }
    return 0;
}


void* receive_thread(void* arg) {
    
    mycord_msg_t msg;
    char time_str[64];
    char mention_target[USERNAME_SIZE + 2];
    snprintf(mention_target, sizeof(mention_target), "@%s", my_username);

    while (running) {
        if (perform_full_read(sockfd, &msg, sizeof(msg)) < 0) {
            if (running) {
		if(config.tui) disable_raw_mode();
                print_error("Server closed connection");
                exit(1);
            }
            break;
        }

        uint32_t type = ntohl(msg.type);
        time_t ts = (time_t)ntohl(msg.timestamp);
        struct tm *tm_info = localtime(&ts);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

	char buffer[MESSAGE_SIZE + 200];

        switch (type) {
            case MESSAGE_RECV: {
                printf("[%s] %s: ", time_str, msg.username);
                char *found = strstr(msg.message, mention_target);
                if (!config.quiet && found) {
                    if (config.tui) printf("\a"); // Manual Beep for TUI

		    int prefix_len = found - msg.message;
                    snprintf(buffer, sizeof(buffer), "[%s] %s: %.*s%s%s%s%s",
                        time_str, msg.username, prefix_len, msg.message,
                        COLOR_RED, mention_target, COLOR_RESET,
                        found + strlen(mention_target));
                } else {
                    snprintf(buffer, sizeof(buffer), "[%s] %s: %s", time_str, msg.username, msg.message);
                }
                add_to_history(buffer);
                break;
            }
            case SYSTEM:
                snprintf(buffer, sizeof(buffer), "%s[SYSTEM] %s%s", COLOR_GRAY, msg.message, COLOR_RESET);
                add_to_history(buffer);
                break;
            case DISCONNECT:
                snprintf(buffer, sizeof(buffer), "%s[DISCONNECT] %s%s", COLOR_RED, msg.message, COLOR_RESET);
                add_to_history(buffer);
                running = 0;
                handle_signal(0);
                break;
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    //Signals
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    //Setup
    if (process_args(argc, argv) != 0) {
        fprintf(stderr, "Error: Invalid arguments\n");
        return 1;
    }
    get_username();

    //Connect
    if ((sockfd = setup_connection()) < 0) return 1;

    mycord_msg_t login = {0};
    login.type = htonl(LOGIN);
    strncpy(login.username, my_username, sizeof(login.username) - 1);
    if (write(sockfd, &login, sizeof(login)) < 0) return 1;

    // Enable TUI if requested
    if (config.tui) enable_raw_mode();
    tui_redraw(); // Initial draw

    pthread_t r_thread;
    pthread_create(&r_thread, NULL, receive_thread, NULL);
    pthread_detach(r_thread);

    // Main Loop
    if (!config.tui) {
        // --- Standard Mode (Use getline) ---
        char *line = NULL; size_t len = 0;
        while (running) {
            ssize_t n = getline(&line, &len, stdin);
            if (n == -1) break;
            if (n > 0 && line[n-1] == '\n') line[--n] = '\0';
            if (n == 0 || n >= MESSAGE_SIZE) continue;
            
            bool valid = true;
            for(int i=0; i<n; i++) if(!isprint(line[i])) valid=false;
            if(!valid) continue;

            mycord_msg_t send_msg = {0};
            send_msg.type = htonl(MESSAGE_SEND);
            strncpy(send_msg.message, line, sizeof(send_msg.message) - 1);
            write(sockfd, &send_msg, sizeof(send_msg));
        }
        free(line);
    } else {
        // --- TUI Mode (Read raw keypresses) ---
        while (running) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 127 || c == 8) { // Backspace
                    if (input_len > 0) input_buffer[--input_len] = '\0';
                } else if (c == '\n' || c == '\r') { // Enter
                    if (input_len > 0) {
                        mycord_msg_t send_msg = {0};
                        send_msg.type = htonl(MESSAGE_SEND);
                        strncpy(send_msg.message, input_buffer, sizeof(send_msg.message) - 1);
                        write(sockfd, &send_msg, sizeof(send_msg));
                        
                        input_len = 0;
                        input_buffer[0] = '\0';
                    }
                } else if (c == '\033') { // Escape sequence (Arrow Keys)
                    char seq[3];
                    if (read(STDIN_FILENO, &seq[0], 1) == 0) continue;
                    if (read(STDIN_FILENO, &seq[1], 1) == 0) continue;
                    if (seq[0] == '[') {
                        if (seq[1] == 'A') { // Up Arrow
                           if (history_count > 0 && scroll_offset < history_count - 1) scroll_offset++;
                        } else if (seq[1] == 'B') { // Down Arrow
                           if (scroll_offset > 0) scroll_offset--;
                        }
                    }
                } else if (isprint(c)) {
                    if (input_len < MESSAGE_SIZE - 1) {
                        input_buffer[input_len++] = c;
                        input_buffer[input_len] = '\0';
                    }
                }
                tui_redraw(); // Redraw UI after every keystroke
            }
        }
    }

    handle_signal(0);
    return 0;
    
}
