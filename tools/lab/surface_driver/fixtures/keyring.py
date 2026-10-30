"""Guest-only synthetic collections; never change a desktop's default wallet.

KDE's compatibility service exposes Secret Service but not GNOME's private
collection control API. Create a unique blank wallet through KWallet's PAM
interface, then use libsecret for this private resident only. The real desktop
bus/portal and its default aliases remain intact.
"""

KDE_CREATE = '''
# Activate the KDE compatibility service, not org.freedesktop.secrets (which
# ksecretd does not advertise as activatable).
bus.call_sync('org.kde.secretservicecompat','/','org.freedesktop.DBus.Peer','Ping',None,None,Gio.DBusCallFlags.NONE,20000,None)
wallet=LABEL
assert wallet.startswith('putgd-fixture-') and len(wallet)==46
wallets=call('/ksecretd','org.kde.KWallet','wallets',None,'(as)')[0]
assert wallet not in wallets
folder=Path.home()/'.local/share/kwalletd'
folder.mkdir(mode=0o700,parents=True,exist_ok=True)
salt=folder/(wallet+'.salt')
wallet_file=folder/(wallet+'.kwl')
assert not wallet_file.exists() and not salt.exists()
import hashlib,time,stat
with open(os.open(salt,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'wb') as f:f.write(os.urandom(56))
identity=salt.stat()
digest=hashlib.pbkdf2_hmac('sha512',b'',salt.read_bytes(),50000,dklen=56)
try:
 call('/ksecretd','org.kde.KWallet','pamOpen',GLib.Variant('(sayi)',(wallet,digest,0)),'()')
 deadline=time.monotonic()+20
 while wallet not in call('/ksecretd','org.kde.KWallet','wallets',None,'(as)')[0]:
  assert time.monotonic()<deadline,'KWallet did not create the fixture wallet'
  time.sleep(.1)
 collection='/org/freedesktop/secrets/collection/'+wallet.replace('-','_2d')
 # Resolve only our exact label: KDE's object-path escaping varies by version.
 candidates=call(service,'org.freedesktop.DBus.Properties','Get',GLib.Variant('(ss)',('org.freedesktop.Secret.Service','Collections')),'(v)')[0]
 found=[p for p in candidates if call(p,'org.freedesktop.DBus.Properties','Get',GLib.Variant('(ss)',('org.freedesktop.Secret.Collection','Label')),'(v)')[0]==wallet]
 assert len(found)==1
 collection=found[0]
except BaseException:
 # A partial creation may have a wallet: remove only the unique owned name.
 if wallet in call('/ksecretd','org.kde.KWallet','wallets',None,'(as)')[0]:
  assert call('/ksecretd','org.kde.KWallet','deleteWallet',GLib.Variant('(s)',(wallet,)),'(i)')[0]==0
 if salt.exists():
  current=salt.lstat();assert (current.st_dev,current.st_ino)==(identity.st_dev,identity.st_ino)
  salt.unlink()
 raise
'''

KDE_SALT_CLEANUP = '''
import stat
p=Path.home()/'.local/share/kwalletd'/(LABEL+'.salt')
if p.exists():
 s=p.lstat()
 assert stat.S_ISREG(s.st_mode) and s.st_uid==os.getuid() and not s.st_mode&0o077
 assert (s.st_dev,s.st_ino)==(SALT_DEVICE,SALT_INODE)
 p.unlink()
'''
