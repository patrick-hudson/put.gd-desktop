"""Values for the sensitive-text fuzz scenes (#1931): sensitive items and look-alikes.

Every value is random and fake, built at run time. Secret prefixes are joined from pieces
here so that no complete secret-shaped literal sits in the source: gitleaks scans every
branch of the repository.

A labelled value is a `Val(text, label, kind)`:
- label 'secret': the check should cover it (kind is the Quick redact category that should
  find it, or 'gap' for a secret no category targets);
- label 'decoy': ordinary content that looks sensitive (a cover is a false positive);
- label 'neutral': covering it is acceptable but not required (an IMEI, a MAC address).
"""
import base64
import hashlib
import json
import string
from dataclasses import dataclass

UP = string.ascii_uppercase
LO = string.ascii_lowercase
DIG = string.digits
ALNUM = UP + LO + DIG
HEX = '0123456789abcdef'
B58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
B64 = ALNUM + '+/'
B64URL = ALNUM + '-_'


@dataclass
class Val:
    text: str
    label: str = 'decoy'
    kind: str = ''
    tag: str = ''


def rs(R, alphabet, n):
    return ''.join(R.choice(alphabet) for _ in range(n))


def p(*parts):
    """Join prefix pieces (keeps whole secret prefixes out of the source)."""
    return ''.join(parts)


# ----- people -------------------------------------------------------------------------------
# The scene's own user (login and real name feed the `name` category).
USERS = [
    ('mrivera', 'Maria Rivera'), ('tnguyen', 'Thanh Nguyen'), ('jsmith', 'John Smith'),
    ('aokafor', 'Adaeze Okafor'), ('kmueller', 'Katrin Müller'), ('soconnor', "Sean O'Connor"),
    ('dkowalski', 'Dorota Kowalski'), ('rpatel', 'Rahul Patel'), ('egarcia', 'Elena Garcia'),
    ('hsato', 'Hiroshi Sato'), ('lbernard', 'Lucie Bernard'), ('obrien', 'Aoife Brien'),
    ('mjohansson', 'Magnus Johansson'), ('fabiano', 'Fabiano Costa'), ('ychen', 'Yu Chen'),
    ('willg', 'Will Grant'), ('amber', 'Amber Stone'), ('mark.rose', 'Mark Rose'),
]
FIRST = ['Alex', 'Sam', 'Priya', 'Jonas', 'Mei', 'Omar', 'Lena', 'Diego', 'Ana', 'Ivan', 'Zoe', 'Kenji',
         'Fatima', 'Lucas', 'Noah', 'Emma', 'Chloe', 'Tariq', 'Nora', 'Ravi', 'Sofia', 'Ben', 'Ines', 'Hugo',
         'Leila', 'Mateo', 'Yara', 'Felix', 'Hana', 'Oskar']
LAST = ['Lopez', 'Kim', 'Schmidt', 'Silva', 'Nakamura', 'Haddad', 'Novak', 'Dubois', 'Rossi', 'Andersen',
        'Kowal', 'Mensah', 'Ivanova', 'Okafor', 'Brennan', 'Moreau', 'Tanaka', 'Fischer', 'Costa', 'Ahmed',
        'Weber', 'Park', 'Larsen', 'Petrov', 'Romano', 'Sato', 'Jansen', 'Varga', 'Quinn', 'Reyes']
COMPANIES = ['acme', 'globex', 'initech', 'umbrella', 'hooli', 'vandelay', 'stark', 'wayne', 'cyberdyne',
             'tyrell', 'soylent', 'massive', 'oscorp', 'aperture', 'blackmesa', 'contoso', 'fabrikam', 'northwind']
TLDS = ['com', 'io', 'org', 'net', 'dev', 'co.uk', 'de', 'fr', 'app', 'ai', 'engineering', 'cloud', 'co', 'eu']
WORDS = ('the and for with this that from your have will about more when what time some them other into than '
         'then only also back after work first well even want because these give most data team build test '
         'release review update deploy server client report meeting project design support issue change plan '
         'note draft budget order invoice account status ready later today morning thanks please check sync '
         'notes agenda migrate cluster backup restore window screen photo album track music level score '
         'match round player season table league final group city map route street coffee lunch dinner '
         'will mark grant amber rose stone max page long bell green').split()
BIP39 = ('abandon ability able about above absent absorb abstract absurd abuse access accident account accuse '
         'achieve acid acoustic acquire across act action actor actress actual adapt add addict address adjust '
         'admit adult advance advice aerobic affair afford afraid again age agent agree ahead aim air airport '
         'aisle alarm album alcohol alert alien all alley allow almost alone alpha already also alter always '
         'amateur amazing among amount amused analyst anchor ancient anger angle angry animal ankle announce '
         'annual another answer antenna antique anxiety any apart apology appear apple approve april arch arctic '
         'area arena argue arm armed armor army around arrange arrest arrive arrow art artefact artist artwork '
         'ask aspect assault asset assist assume asthma athlete atom attack attend attitude attract auction '
         'audit august aunt author auto autumn average avocado avoid awake aware away awesome awful awkward '
         'axis baby bachelor bacon badge bag balance balcony ball bamboo banana banner bar barely bargain '
         'barrel base basic basket battle beach bean beauty because become beef before begin behave behind '
         'believe below belt bench benefit best betray better between beyond bicycle bid bike bind biology '
         'bird birth bitter black blade blame blanket blast bleak bless blind blood blossom blouse blue blur '
         'blush board boat body boil bomb bone bonus book boost border boring borrow boss bottom bounce box').split()


def person(R):
    return R.choice(FIRST), R.choice(LAST)


def email(R, first=None, last=None, label='secret'):
    first = first or R.choice(FIRST)
    last = last or R.choice(LAST)
    f, l = first.lower(), last.lower().replace("'", '')
    co = R.choice(COMPANIES)
    if R.random() < 0.04:
        f = R.choice(['müller', 'josé', 'zoë', 'françois', 'søren', 'łukasz'])
    local = R.choice([f'{f}.{l}', f'{f[0]}{l}', f'{f}', f'{f}_{l}', f'{f}.{l}{R.randint(1, 99)}',
                      f'{f}+{R.choice(["news", "github", "billing", "test"])}', f'{l}.{f[0]}', f'{f}-{l}'])
    domain = R.choice([f'{co}.{R.choice(TLDS)}', f'mail.{co}.com', f'{co}-{R.choice(["labs", "group", "corp"])}.{R.choice(TLDS)}',
                       R.choice(['gmail.com', 'outlook.com', 'proton.me', 'yahoo.co.jp', 'web.de', 'fastmail.fm'])])
    return Val(f'{local}@{domain}', label, 'email')


def phone(R, country=None):
    c = country or R.choice(['us', 'us', 'uk', 'de', 'fr', 'jp', 'cn', 'in', 'br', 'au', 'es', 'it', 'nl', 'ru', 'kr'])
    d = lambda n: rs(R, DIG, n)
    nz = lambda: R.choice('23456789')
    forms = {
        'us': [f'({nz()}{d(2)}) 555-{d(4)}', f'+1 {nz()}{d(2)}-555-{d(4)}', f'{nz()}{d(2)}.555.{d(4)}', f'+1 ({nz()}{d(2)}) 555 {d(4)}', f'{nz()}{d(2)}-555-{d(4)}'],
        'uk': [f'+44 20 {d(4)} {d(4)}', f'020 {d(4)} {d(4)}', f'07700 {d(6)}', f'+44 7700 {d(6)}', f'07700{d(6)}'],
        'de': [f'+49 30 {d(8)}', f'030 {d(4)} {d(4)}', f'+49 (0)30 {d(4)} {d(4)}', f'0151 {d(8)}', f'+49 151 {d(4)} {d(4)}', f'030/{d(7)}', f'+49 30 / {d(6)}'],
        'fr': [f'06 {d(2)} {d(2)} {d(2)} {d(2)}', f'+33 6 {d(2)} {d(2)} {d(2)} {d(2)}', f'01 {d(2)} {d(2)} {d(2)} {d(2)}', f'06.{d(2)}.{d(2)}.{d(2)}.{d(2)}'],
        'jp': [f'090-{d(4)}-{d(4)}', f'+81 90-{d(4)}-{d(4)}', f'03-{d(4)}-{d(4)}'],
        'cn': [f'138 {d(4)} {d(4)}', f'13{d(9)}', f'+86 139 {d(4)} {d(4)}', f'+86 13{d(9)}'],
        'in': [f'+91 98{d(3)} {d(5)}', f'98{d(3)} {d(5)}', f'+91-98{d(8)}'],
        'br': [f'(11) 9{d(4)}-{d(4)}', f'+55 11 9{d(4)}-{d(4)}'],
        'au': [f'0412 {d(3)} {d(3)}', f'+61 412 {d(3)} {d(3)}', f'(02) {d(4)} {d(4)}'],
        'es': [f'6{d(2)} {d(2)} {d(2)} {d(2)}', f'+34 6{d(2)} {d(3)} {d(3)}'],
        'it': [f'+39 3{d(2)} {d(3)} {d(4)}', f'3{d(2)} {d(7)}'],
        'nl': [f'06-{d(8)}', f'+31 6 {d(8)}'],
        'ru': [f'+7 (9{d(2)}) {d(3)}-{d(2)}-{d(2)}', f'8 9{d(2)} {d(3)}-{d(2)}-{d(2)}'],
        'kr': [f'010-{d(4)}-{d(4)}', f'+82 10-{d(4)}-{d(4)}'],
    }
    return Val(R.choice(forms[c]), 'secret', 'phone', c)


def ipv4(R, label='secret'):
    form = R.random()
    if form < 0.4:
        a = R.choice([f'10.{R.randint(0, 255)}.{R.randint(0, 255)}.{R.randint(1, 254)}',
                      f'192.168.{R.randint(0, 255)}.{R.randint(1, 254)}', f'172.{R.randint(16, 31)}.{R.randint(0, 255)}.{R.randint(1, 254)}'])
    else:
        a = f'{R.randint(1, 223)}.{R.randint(0, 255)}.{R.randint(0, 255)}.{R.randint(1, 254)}'
    t = R.random()
    if t < 0.2:
        a += f':{R.choice([22, 80, 443, 3000, 5432, 6379, 8080, 8443, 9000, 51820])}'
    elif t < 0.3:
        a += f'/{R.choice([8, 16, 24, 32])}'
    return Val(a, label, 'ip')


def ipv6(R):
    g = lambda: format(R.randint(0, 0xffff), 'x')
    forms = [f'2001:db8:{g()}:{g()}::{g()}', f'fe80::{g()}:{g()}:{g()}:{g()}', f'2a02:{g()}:{g()}:{g()}:{g()}:{g()}:{g()}:{g()}',
             f'[2001:db8::{g()}]:443', f'fd00::{g()}', f'2600:1f18:{g()}:{g()}:{g()}:{g()}:{g()}:{g()}', f'::ffff:192.0.2.{R.randint(1, 254)}']
    return Val(R.choice(forms), 'secret', 'ip')


def luhn_digit(body):
    total = 0
    for i, ch in enumerate(reversed(body)):
        n = int(ch)
        if i % 2 == 0:
            n *= 2
            if n > 9:
                n -= 9
        total += n
    return str((10 - total % 10) % 10)


def luhn_ok(digits):
    return luhn_digit(digits[:-1]) == digits[-1]


def card_digits(R, brand=None):
    brand = brand or R.choice(['visa', 'visa', 'mc', 'mc', 'amex', 'disc'])
    if brand == 'visa':
        body = '4' + rs(R, DIG, 14)
    elif brand == 'mc':
        body = R.choice(['51', '52', '53', '54', '55']) + rs(R, DIG, 13)
    elif brand == 'amex':
        body = R.choice(['34', '37']) + rs(R, DIG, 12)
    else:
        body = '6011' + rs(R, DIG, 11)
    return body + luhn_digit(body)


def group_card(digits, sep=' '):
    if len(digits) == 15:
        return sep.join([digits[:4], digits[4:10], digits[10:]])
    return sep.join(digits[i:i + 4] for i in range(0, len(digits), 4))


def card(R, sep=None):
    d = card_digits(R)
    sep = sep if sep is not None else R.choice([' ', ' ', '-', ''])
    return Val(group_card(d, sep) if sep else d, 'secret', 'card', 'unbroken' if not sep else 'grouped')


# ----- keys and tokens (assembled at run time) ----------------------------------------------
def b64json(obj):
    return base64.urlsafe_b64encode(json.dumps(obj, separators=(',', ':')).encode()).rstrip(b'=').decode()


def jwt(R):
    head = b64json({'alg': R.choice(['HS256', 'RS256', 'ES256']), 'typ': 'JWT'})
    body = b64json({'sub': rs(R, DIG, 8), 'name': R.choice(FIRST), 'iat': 1700000000 + R.randint(0, 9999999),
                    'role': R.choice(['admin', 'user', 'svc'])})
    return f'{head}.{body}.{rs(R, B64URL, 43)}'


def token(R, kind=None):
    k = kind or R.choice(['aws', 'aws_secret', 'gh', 'gh_fine', 'gl', 'slack', 'stripe', 'openai', 'anthropic',
                          'google', 'hf', 'npm', 'sendgrid', 'jwt', 'hex40', 'b64_40', 'twilio_sid_secret'])
    if k == 'aws':
        t = p('AK', 'IA') + rs(R, UP + '234567', 16)
    elif k == 'aws_secret':
        t = rs(R, ALNUM + '/+', 40)
    elif k == 'gh':
        t = p('gh', R.choice('pousr'), '_') + rs(R, ALNUM, 36)
    elif k == 'gh_fine':
        t = p('github', '_pat_') + rs(R, ALNUM, 22) + '_' + rs(R, ALNUM, 59)
    elif k == 'gl':
        t = p('gl', 'pat-') + rs(R, ALNUM + '_-', 20)
    elif k == 'slack':
        t = p('xo', 'xb-') + rs(R, DIG, 11) + '-' + rs(R, DIG, 13) + '-' + rs(R, ALNUM, 24)
    elif k == 'stripe':
        t = p('sk', '_live_') + rs(R, ALNUM, 99 if R.random() < 0.3 else 24)
    elif k == 'openai':
        t = p('sk', '-proj-') + rs(R, B64URL, 156)
    elif k == 'anthropic':
        t = p('sk', '-ant-', 'api03-') + rs(R, B64URL, 93) + 'AA'
    elif k == 'google':
        t = p('AI', 'za') + rs(R, B64URL, 35)
    elif k == 'hf':
        t = p('h', 'f_') + rs(R, ALNUM, 34)
    elif k == 'npm':
        t = p('np', 'm_') + rs(R, ALNUM, 36)
    elif k == 'sendgrid':
        t = p('S', 'G.') + rs(R, B64URL, 22) + '.' + rs(R, B64URL, 43)
    elif k == 'jwt':
        t = jwt(R)
    elif k == 'hex40':
        t = rs(R, HEX, 40)
    elif k == 'b64_40':
        t = rs(R, B64, 40)
    elif k == 'discord':
        t = base64.b64encode(rs(R, DIG, 18).encode()).decode().rstrip('=') + '.' + rs(R, B64URL, 6) + '.' + rs(R, B64URL, 38)
    elif k == 'tailscale':
        t = p('tskey', '-auth-') + rs(R, ALNUM, 17) + '-' + rs(R, ALNUM, 33)
    elif k == 'age':
        t = p('AGE-SECRET', '-KEY-1') + rs(R, 'QPZRY9X8GF2TVDW0S3JN54KHCE6MUA7L', 58)
    elif k == 'telegram':
        t = rs(R, DIG, 10) + ':' + p('AA') + rs(R, B64URL, 33)
    elif k == 'kubeadm':
        t = rs(R, LO + DIG, 6) + '.' + rs(R, LO + DIG, 16)
    elif k == 'base32':
        t = rs(R, 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567', 32)
    else:  # a 32-hex secret with no prefix of its own (Twilio auth token and the like)
        t = rs(R, HEX, 32)
    return Val(t, 'secret', 'key', k)


def pem_lines(R, n=None):
    kind = R.choice(['RSA PRIVATE KEY', 'PRIVATE KEY', 'EC PRIVATE KEY', 'OPENSSH PRIVATE KEY'])
    n = n or R.randint(4, 9)
    return ['-----' + p('BEGIN ', kind) + '-----'] + [rs(R, B64, 64) for _ in range(n)] + [rs(R, B64, R.randint(8, 60)) + '==', '-----' + p('END ', kind) + '-----']


def password(R, strong=True):
    if strong:
        return rs(R, ALNUM + '!#$%&*+-=?@^_', R.randint(14, 24))
    return R.choice(['Summer', 'Winter', 'Falcon', 'Purple', 'Monkey', 'Dragon', 'Sunshine']) + str(R.randint(10, 9999)) + R.choice(['!', '#', '', '?'])


# ----- look-alikes --------------------------------------------------------------------------
def uuid(R):
    h = rs(R, HEX, 32)
    return f'{h[:8]}-{h[8:12]}-4{h[13:16]}-{R.choice("89ab")}{h[17:20]}-{h[20:]}'


def sha(R, n=40):
    return rs(R, HEX, n)


def b58(R, n):
    return rs(R, B58, n)


def version(R):
    return R.choice([f'{R.randint(0, 9)}.{R.randint(0, 30)}.{R.randint(0, 20)}', f'{R.randint(1, 4)}.{R.randint(0, 20)}.{R.randint(0, 9)}.{R.randint(0, 99)}',
                     f'v{R.randint(1, 3)}.{R.randint(0, 12)}.{R.randint(0, 9)}', f'{R.randint(20, 26)}.{R.randint(1, 12)}.{R.randint(0, 9)}',
                     f'{R.randint(1, 9)}.{R.randint(0, 9)}.{R.randint(0, 9)}-{R.choice(["rc1", "beta.2", "alpha", "1ubuntu2", "3.fc45"])}'])


def isbn13(R):
    body = '978' + rs(R, DIG, 9)
    total = sum(int(c) * (1 if i % 2 == 0 else 3) for i, c in enumerate(body))
    check = str((10 - total % 10) % 10)
    d = body + check
    return f'{d[:3]}-{d[3]}-{d[4:6]}-{d[6:12]}-{d[12]}'


def ean13(R):
    body = rs(R, DIG, 12)
    total = sum(int(c) * (1 if i % 2 == 0 else 3) for i, c in enumerate(body))
    return body + str((10 - total % 10) % 10)


def imei(R):
    body = R.choice(['35', '86', '01', '49']) + rs(R, DIG, 12)
    return body + luhn_digit(body)


def mac(R, sep=':'):
    return sep.join(format(R.randint(0, 255), '02x') for _ in range(6))


def sha256_of(text):
    return hashlib.sha256(text.encode()).hexdigest()


def money(R, locale='en'):
    v = R.randint(1000, 9999999)
    cents = R.randint(0, 99)
    if locale == 'fr':
        s = f'{v:,}'.replace(',', ' ')
        return f'{s},{cents:02d} €'
    if locale == 'de':
        return f'{v:,}'.replace(',', '.') + f',{cents:02d} €'
    if locale == 'ch':
        return f"CHF {v:,}".replace(',', "'") + f'.{cents:02d}'
    if locale == 'in':
        s = str(v)
        head, tail = s[:-3], s[-3:]
        parts = []
        while len(head) > 2:
            parts.insert(0, head[-2:])
            head = head[:-2]
        if head:
            parts.insert(0, head)
        return '₹' + ','.join(parts + [tail]) if parts else '₹' + tail
    return f'${v:,}.{cents:02d}'


def sentence(R, n=None):
    n = n or R.randint(4, 10)
    w = [R.choice(WORDS) for _ in range(n)]
    w[0] = w[0].capitalize()
    return ' '.join(w)
