"""Type the lab guest's password into the LightDM field over VNC; the password is never printed."""
import sys, time
sys.path.insert(0, '/home/patrick/work/put.gd/.claude/worktrees/recursing-feynman-beef82/tools/lab')
import rfb
secret = open('/home/patrick/work/put.gd/work/lab-bootstrap/install/password').read().strip('\n')
c = rfb.RFB(5998)
c.click(260, 409); time.sleep(.3)
c.type(secret); time.sleep(.2); c.key('ret')
c.close(); print('typed')
