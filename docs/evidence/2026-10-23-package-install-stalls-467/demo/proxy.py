"""Throwaway #467 demo: an HTTP proxy for apt that accepts every connection.

While the mode file says "stall" it answers with headers and then one body byte
every 15 s, forever: apt's own 60 s idle timeout never fires and apt prints
nothing, like nightly run 39851369595. In "relay" mode it forwards to the mirror.
"""
import socket
import sys
import threading
import time

MODE = sys.argv[1]


def pump(source, target):
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            target.sendall(data)
    except OSError:
        pass
    finally:
        for sock in (source, target):
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def handle(client):
    try:
        head = b''
        while b'\r\n\r\n' not in head:
            chunk = client.recv(65536)
            if not chunk:
                return
            head += chunk
        mode = open(MODE).read().strip()
        url = head.split(b'\r\n', 1)[0].split()[1].decode()
        print(f'{time.strftime("%H:%M:%S")} {mode}: {url}', flush=True)
        if mode == 'stall':
            client.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 1048576\r\n\r\n')
            while True:
                time.sleep(15)
                client.sendall(b'\0')
        host, _, port = url.split('/')[2].partition(':')
        upstream = socket.create_connection((host, int(port or 80)), timeout=60)
        upstream.settimeout(None)
        upstream.sendall(head)
        threading.Thread(target=pump, args=(upstream, client), daemon=True).start()
        pump(client, upstream)
    except OSError:
        pass


server = socket.create_server(('127.0.0.1', 3128))
while True:
    connection, _ = server.accept()
    threading.Thread(target=handle, args=(connection,), daemon=True).start()
