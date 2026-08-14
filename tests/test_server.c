/*
 * tests/test_server.c — throwaway local HTTP server (python3, tests only).
 */

#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_server.h"

static const char* PY_SERVER = "import http.server, socketserver, sys\n"
                               "PORT = int(sys.argv[1])\n"
                               "BODY = sys.argv[2]\n"
                               "STATUS = int(sys.argv[3])\n"
                               "class H(http.server.BaseHTTPRequestHandler):\n"
                               "    def send_body(self):\n"
                               "        self.send_response(STATUS)\n"
                               "        self.send_header('Content-Type', 'application/json')\n"
                               "        self.send_header('Content-Length', str(len(BODY.encode('utf-8'))))\n"
                               "        self.end_headers()\n"
                               "        for i in range(0, len(BODY), 7):\n"
                               "            self.wfile.write(BODY[i:i+7].encode('utf-8'))\n"
                               "            self.wfile.flush()\n"
                               "    def do_POST(self): self.send_body()\n"
                               "    def do_GET(self): self.send_body()\n"
                               "    def log_message(self, *a): pass\n"
                               "socketserver.TCPServer(('127.0.0.1', PORT), H).serve_forever()\n";

int test_server_find_free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &len) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return ntohs(addr.sin_port);
}

pid_t test_server_start(int port, const char* body, int status) {
    char port_s[16], status_s[8];
    snprintf(port_s, sizeof(port_s), "%d", port);
    snprintf(status_s, sizeof(status_s), "%d", status);

    pid_t pid = fork();
    if (pid == 0) {
        execlp("python3", "python3", "-c", PY_SERVER, port_s, body, status_s, (char*)NULL);
        _exit(127);
    }
    return pid;
}

int test_server_wait(int port, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons((uint16_t)port);
            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                close(fd);
                return 0;
            }
            close(fd);
        }
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return -1;
}

void test_server_stop(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
    }
}
