#!/usr/bin/env python3
"""Loopback TCP proxy that caps the client-to-server rate, shared by all
connections, like a slow uplink in front of a local S3 server (moto).

  throttle_proxy.py LISTEN_PORT TARGET_PORT BYTES_PER_SECOND

Prints the port once listening. Server-to-client traffic is not limited.
"""
import asyncio
import sys
import time

LISTEN, TARGET, RATE = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])


class Bucket:
    def __init__(self, rate):
        self.rate, self.tokens, self.last = rate, 0.0, time.monotonic()
        self.lock = asyncio.Lock()

    async def take(self, amount):
        async with self.lock:
            while True:
                now = time.monotonic()
                self.tokens = min(self.rate * 0.05, self.tokens + (now - self.last) * self.rate)
                self.last = now
                if self.tokens >= amount:
                    self.tokens -= amount
                    return
                await asyncio.sleep((amount - self.tokens) / self.rate)


bucket = Bucket(RATE)


async def pipe(reader, writer, limited):
    try:
        while True:
            data = await reader.read(16384 if limited else 65536)
            if not data:
                break
            if limited:
                await bucket.take(len(data))
            writer.write(data)
            await writer.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def handle(client_reader, client_writer):
    try:
        server_reader, server_writer = await asyncio.open_connection('127.0.0.1', TARGET)
    except OSError:
        client_writer.close()
        return
    await asyncio.gather(pipe(client_reader, server_writer, True), pipe(server_reader, client_writer, False))


async def main():
    server = await asyncio.start_server(handle, '127.0.0.1', LISTEN)
    print(server.sockets[0].getsockname()[1], flush=True)
    async with server:
        await server.serve_forever()


asyncio.run(main())
