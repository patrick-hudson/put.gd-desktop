#!/usr/bin/env python3
"""Bounded wrapper around the unchanged round9 throttle proxy, or a stall.

The original token cap is RATE*.05 (13107 bytes at 256KiB/s), smaller than
its 16384-byte reads. This wrapper raises that cap to the requested chunk
size. All original connection forwarding and shared rate accounting remain.
No system networking settings change. Only literal IPv4 loopback listeners.
"""
import argparse
import ast
import asyncio
import hashlib
from pathlib import Path
import sys
import time


def load_original(path, listen, target, rate):
    source = path.read_bytes()
    tree = ast.parse(source)
    final = tree.body[-1]
    if not (isinstance(final, ast.Expr) and ast.dump(final.value) == ast.dump(
            ast.parse('asyncio.run(main())').body[0].value)):
        raise ValueError('unrecognized_frozen_proxy_startup')
    tree.body.pop()  # Load definitions without starting the original event loop.
    previous = sys.argv
    try:
        sys.argv = [str(path), str(listen), str(target), str(rate)]
        namespace = {'__name__': 'frozen_round9_proxy', '__file__': str(path)}
        exec(compile(tree, str(path), 'exec'), namespace)
    finally:
        sys.argv = previous
    return namespace, hashlib.sha256(source).hexdigest()


class BoundedBucket:
    def __init__(self, rate):
        if not 1 <= rate <= 1024**3:
            raise ValueError('invalid_rate')
        self.rate, self.tokens, self.last = rate, 0., time.monotonic()
        self.lock = asyncio.Lock()

    async def take(self, amount):
        if not 0 < amount <= 16384:
            raise ValueError('unrecognized_original_chunk_size')
        async with self.lock:
            while True:
                now = time.monotonic()
                self.tokens = min(max(self.rate * .05, amount), self.tokens + (now - self.last) * self.rate)
                self.last = now
                if self.tokens >= amount:
                    self.tokens -= amount
                    return
                await asyncio.sleep((amount - self.tokens) / self.rate)


async def stalled(reader, writer):
    # No upstream connection and no draining of the client's socket. Keep it
    # stalled until this owned subprocess is terminated by the supervisor.
    try:
        await asyncio.Event().wait()
    finally:
        writer.close()


async def main(args):
    if args.stall:
        server = await asyncio.start_server(stalled, '127.0.0.1', args.listen)
        print(server.sockets[0].getsockname()[1], flush=True)
        async with server:
            await server.serve_forever()
    else:
        original, _digest = load_original(args.source, args.listen, args.target, args.rate)
        original['bucket'] = BoundedBucket(args.rate)
        await original['main']()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--listen', type=int, default=0)
    parser.add_argument('--target', type=int, required=True)
    parser.add_argument('--rate', type=int, default=262144)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--stall', action='store_true')
    args = parser.parse_args()
    if not 0 <= args.listen <= 65535 or not 1 <= args.target <= 65535:
        parser.error('invalid_loopback_port')
    asyncio.run(main(args))
