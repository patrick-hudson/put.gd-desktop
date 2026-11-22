/* putgd-sftp-askpass (#1871): the SSH_ASKPASS program putgd-sftp-worker gives the user's
 * ssh. ssh runs it with the prompt as its only argument; it asks the worker over the
 * single-use Unix socket the worker named, with the worker's one-time token, and prints
 * the one answer it gets. It holds no secret itself and links only the C library, so it
 * starts under the host's own library path when put.gd runs from its AppImage. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int writeAll(int fd, const char* data, size_t size) {
    while (size > 0) {
        ssize_t count = write(fd, data, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        data += count;
        size -= (size_t)count;
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* socketPath = getenv("PUTGD_SFTP_ASKPASS_SOCKET");
    const char* token = getenv("PUTGD_SFTP_ASKPASS_TOKEN");
    const char* kind = getenv("SSH_ASKPASS_PROMPT"); /* "none": a notice; "confirm": yes or no */
    /* ssh may exit before it reads the answer: the failed write then ends this program with status 1. */
    signal(SIGPIPE, SIG_IGN);
    if (argc != 2 || !socketPath || !token || strlen(socketPath) >= sizeof(((struct sockaddr_un*)0)->sun_path)) return 1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 1;
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, socketPath);
    if (connect(fd, (struct sockaddr*)&address, sizeof address) != 0) return 1;
    /* token, kind and prompt, one line each; the prompt's own line breaks become spaces */
    char* prompt = strdup(argv[1]);
    if (!prompt) return 1;
    for (char* c = prompt; *c; ++c) if (*c == '\n' || *c == '\r') *c = ' ';
    if (writeAll(fd, token, strlen(token)) || writeAll(fd, "\n", 1) || writeAll(fd, kind ? kind : "", kind ? strlen(kind) : 0) ||
        writeAll(fd, "\n", 1) || writeAll(fd, prompt, strlen(prompt)) || writeAll(fd, "\n", 1))
        return 1;
    shutdown(fd, SHUT_WR);
    /* "ok\n" and the answer, or anything else for no answer */
    char reply[8192];
    size_t have = 0;
    while (have < sizeof reply) {
        ssize_t count = read(fd, reply + have, sizeof reply - have);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        have += (size_t)count;
    }
    close(fd);
    if (have < 3 || memcmp(reply, "ok\n", 3) != 0) {
        memset(reply, 0, sizeof reply);
        return 1;
    }
    int failed = writeAll(STDOUT_FILENO, reply + 3, have - 3) || writeAll(STDOUT_FILENO, "\n", 1);
    memset(reply, 0, sizeof reply);
    return failed ? 1 : 0;
}
