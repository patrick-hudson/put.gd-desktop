"""A TLS front for tests/fixtures/s3_server.py that can reset TLS handshakes (#2051).

    tls_front.py CERT KEY BACKEND_PORT PLAN

Prints its port, then serves HTTPS on 127.0.0.1: it terminates TLS and forwards the bytes to the
plain fixture on BACKEND_PORT. PLAN is a file the test writes and this front reads at each new
connection: "PASS RESET [DROP]" lets PASS more connections through, then answers the next RESET ones
by reading their ClientHello and resetting the connection (RST), as a network that resets a
handshake does: libcurl reports such a connection as curlCode 35 before any of its request is
written. The next DROP ones complete TLS and forward the request to the fixture, then reset the
connection instead of passing back the reply, as a network that loses the reply of a sent request
does (libcurl's 52 or 56). The front rewrites PLAN as it goes and appends one line to PLAN.log per
connection: "pass", "reset" or "drop".
"""
import select, socket, ssl, struct, sys, threading
from pathlib import Path

cert, key, backend, plan = sys.argv[1], sys.argv[2], int(sys.argv[3]), Path(sys.argv[4])
log = plan.with_name(plan.name + '.log')
lock = threading.Lock()
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(cert, key)


def decide():
    with lock:
        try:
            counts = [int(n) for n in plan.read_text().split()]
            passing, resets, drops = (counts + [0])[:3]
        except (OSError, ValueError):
            passing, resets, drops = 0, 0, 0
        if passing > 0:
            passing -= 1
            verdict = 'pass'
        elif resets > 0:
            resets -= 1
            verdict = 'reset'
        elif drops > 0:
            drops -= 1
            verdict = 'drop'
        else:
            verdict = 'pass'
        plan.write_text('%d %d %d\n' % (passing, resets, drops))
        with log.open('a') as f:
            f.write(verdict + '\n')
        return verdict


def forward(secure, upstream, drop=False):
    """Both directions on one thread: an SSL socket takes no reads and writes from two at once.

    DROP: the first reply bytes from the fixture are not passed on; the client connection is reset.
    """
    try:
        while True:
            ready = [secure] if secure.pending() else select.select([secure, upstream], [], [], 30)[0]
            if not ready:
                return
            for source, target in ((secure, upstream), (upstream, secure)):
                if source in ready:
                    try:
                        data = source.recv(65536)
                    except ssl.SSLWantReadError:
                        # Readable, but not yet a whole TLS record.
                        continue
                    if not data:
                        return
                    if drop and source is upstream:
                        secure.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                        return
                    target.sendall(data)
    except OSError:
        pass
    finally:
        upstream.close()
        secure.close()


def serve(client):
    verdict = decide()
    if verdict == 'reset':
        client.settimeout(5)
        try:
            client.recv(16384)
        except OSError:
            pass
        client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        client.close()
        return
    try:
        secure = context.wrap_socket(client, server_side=True)
    except (OSError, ssl.SSLError):
        client.close()
        return
    forward(secure, socket.create_connection(('127.0.0.1', backend)), drop=verdict == 'drop')


listener = socket.socket()
listener.bind(('127.0.0.1', 0))
listener.listen(64)
print(listener.getsockname()[1], flush=True)
while True:
    connection, _ = listener.accept()
    threading.Thread(target=serve, args=(connection,), daemon=True).start()
