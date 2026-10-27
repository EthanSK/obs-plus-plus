#!/usr/bin/env python3
"""Exercise librtmp socket ownership with a real reused file descriptor."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / "plugins/obs-outputs/librtmp/rtmp.c").read_text()
start = source.index("int\nRTMPSockBuf_Close(")
opening = source.index("{", start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1

fixture = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET -1
#define closesocket close
typedef struct { SOCKET sb_socket; void *sb_ssl; } RTMPSockBuf;
static int shutdowns, tls_closes;
static void TLS_shutdown(void *session) { assert(session); ++shutdowns; }
static void TLS_close(void *session) { assert(session); ++tls_closes; }
'''
fixture += source[start:end]
fixture += r'''
int main(void) {
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    RTMPSockBuf buffer = {.sb_socket = sockets[0]};
#ifdef CRYPTO
    buffer.sb_ssl = &buffer;
#endif
    assert(RTMPSockBuf_Close(&buffer) == 0);
    int replacement = open("/dev/null", O_RDONLY);
    assert(replacement >= 0);
    if (replacement != sockets[0]) {
        assert(dup2(replacement, sockets[0]) == sockets[0]); close(replacement);
        replacement = sockets[0];
    }
    assert(RTMPSockBuf_Close(&buffer) == 0);
    assert(fcntl(replacement, F_GETFD) != -1); // A stale socket number must never close an unrelated newly opened file.
    assert(buffer.sb_socket == INVALID_SOCKET);
    assert(RTMPSockBuf_Close(&buffer) == 0);
#ifdef CRYPTO
    assert(shutdowns == 1 && tls_closes == 1 && buffer.sb_ssl == NULL);
#else
    assert(shutdowns == 0 && tls_closes == 0);
#endif
    close(replacement); close(sockets[1]);
    puts("PASS: repeated RTMP socket close preserves reused descriptors and releases TLS once");
}
'''

with tempfile.TemporaryDirectory(prefix="obs-rtmp-close-test-") as directory:
    harness, binary = Path(directory) / "close.c", Path(directory) / "close"
    harness.write_text(fixture)
    for defines in ([], ["-DCRYPTO"]):
        subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        *defines, str(harness), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], timeout=5, check=True)
