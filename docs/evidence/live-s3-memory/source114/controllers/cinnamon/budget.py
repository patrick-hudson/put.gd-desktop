"""Private shared upload reservations. Importing this module touches no ledger."""
import fcntl, json, os, stat, tempfile, uuid
from pathlib import Path

CAPS = (32, 10485760)
BOUNDS = {'clipboard-text':65536,'clipboard-image':65536,'clipboard-files':131072,
          'region':65536,'window':2097152,'desktop':5242880}

def private_file(fd):
    s=os.fstat(fd)
    if not stat.S_ISREG(s.st_mode) or s.st_uid!=os.getuid() or s.st_mode&0o077 or s.st_nlink!=1:
        raise RuntimeError('budget_file_not_owned_private_regular')

def reserve(ledger, bundle, kind, count):
    ledger=Path(ledger)
    if kind not in BOUNDS or type(count) is not int or not 1<=count<=20:
        raise ValueError('invalid_budget_reservation')
    parent=ledger.parent.resolve();s=parent.stat()
    if s.st_uid!=os.getuid() or s.st_mode&0o077:
        raise RuntimeError('budget_parent_not_owned_private')
    lock=parent/(ledger.name+'.lock')
    fd=os.open(lock,os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW,0o600)
    try:
        private_file(fd);fcntl.flock(fd,fcntl.LOCK_EX)
        try:
            old=os.open(ledger,os.O_RDONLY|os.O_NOFOLLOW)
        except FileNotFoundError:
            value={'maxObjects':CAPS[0],'maxBytes':CAPS[1],'reservedObjects':0,'reservedBytes':0,'attempts':[]}
        else:
            try:
                private_file(old)
                with os.fdopen(old,closefd=False) as stream:value=json.load(stream)
            finally:os.close(old)
        if (value['maxObjects'],value['maxBytes'])!=CAPS:
            raise RuntimeError('budget_caps_changed')
        if any(type(value[k]) is not int or value[k]<0 for k in ('reservedObjects','reservedBytes')) or not isinstance(value['attempts'],list):
            raise RuntimeError('invalid_budget_state')
        objects=count*(2 if kind=='clipboard-files' else 1);size=count*BOUNDS[kind]
        if value['reservedObjects']+objects>CAPS[0] or value['reservedBytes']+size>CAPS[1]:
            raise RuntimeError('live_upload_budget_exhausted')
        reservation={'id':str(uuid.uuid4()),'bundle':str(Path(bundle).resolve()),'kind':kind,'count':count,
                     'reservedObjects':objects,'reservedBytes':size,'intendedOnlyAtWrite':True,'refundable':False}
        value['reservedObjects']+=objects;value['reservedBytes']+=size;value['attempts'].append(reservation)
        output,name=tempfile.mkstemp(prefix='.'+ledger.name+'-',dir=parent)
        try:
            with os.fdopen(output,'w') as stream:
                json.dump(value,stream,indent=2);stream.write('\n');stream.flush();os.fsync(stream.fileno())
            os.replace(name,ledger)
            directory=os.open(parent,os.O_RDONLY|os.O_DIRECTORY)
            try:os.fsync(directory)
            finally:os.close(directory)
        finally:
            if os.path.exists(name):os.unlink(name)
        return reservation
    finally:
        fcntl.flock(fd,fcntl.LOCK_UN);os.close(fd)
