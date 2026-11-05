import sys, time, struct
sys.path.insert(0, '/home/patrick/work/put.gd/.claude/worktrees/recursing-feynman-beef82/tools/lab')
import rfb
c = rfb.RFB(5998)
def right(x, y):
    c.sock.sendall(struct.pack('>BBHH', 5, 0, x, y)); time.sleep(.05)
    c.sock.sendall(struct.pack('>BBHH', 5, 4, x, y)); time.sleep(.08)
    c.sock.sendall(struct.pack('>BBHH', 5, 0, x, y))
def shot(name):
    c.screenshot(f'/tmp/claude-1000/-home-patrick-work-put-gd--claude-worktrees-recursing-feynman-beef82/ecc37013-3db7-4fae-b8f1-4d73bfbe88a0/scratchpad/{name}.png')
for step in sys.argv[1:]:
    op, _, arg = step.partition(':')
    if op == 'click': x, y = map(int, arg.split(',')); c.click(x, y)
    elif op == 'right': x, y = map(int, arg.split(',')); right(x, y)
    elif op == 'move': x, y = map(int, arg.split(',')); c.pointer(x, y)
    elif op == 'type': c.type(arg)
    elif op == 'key': c.key(*arg.split('+'))
    elif op == 'sleep': time.sleep(float(arg))
    elif op == 'shot': shot(arg)
c.close()
