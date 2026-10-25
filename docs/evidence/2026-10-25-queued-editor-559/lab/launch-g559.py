from pathlib import Path
import subprocess,json
root=Path('/home/patrick/work/i559');base=Path('/home/patrick/work/put.gd/work')
pins=(base/'lab-bootstrap-d69/known_hosts-gnome').read_text().splitlines()
line=next(x for x in pins if x.startswith('[127.0.0.1]:22222 '))
(root/'g559-known-hosts').write_text(line.replace('[127.0.0.1]:22222','[127.0.0.1]:22259')+'\n')
subprocess.run(['python3',str(root/'tools/diagnostics/check_socket_paths.py'),'--socket',str(root/'work/lab-runs/g559/qmp.sock')],check=True)
subprocess.run(['python3',str(root/'tools/lab/lab.py'),'launch','--profile','gnome-wayland','--golden',str(base/'lab-images/transfer-0d2922e34f4b/image.qcow2'),'--run',str(root/'work/lab-runs/g559'),'--bootstrap-key',str(base/'lab-bootstrap-d69/identity'),'--known-hosts',str(root/'g559-known-hosts'),'--ssh-port','22259','--vnc-display','59','--layout','1920x1080+0+0'],check=True)
