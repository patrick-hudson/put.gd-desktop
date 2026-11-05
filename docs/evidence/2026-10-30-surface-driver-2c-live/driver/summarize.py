import json, sys
s = json.load(open(sys.argv[1]))
for k, v in s['cases'].items():
    if not v['ok']:
        print(k, v); continue
    val = v['value']; fr = []
    def walk(x):
        if isinstance(x, dict):
            if 'guard' in x and 'label' in x: fr.append(x)
            else: [walk(y) for y in x.values()]
        elif isinstance(x, list): [walk(y) for y in x]
    walk(val)
    print(k, {a: b for a, b in val.items() if not isinstance(b, (list, dict))} if isinstance(val, dict) else val)
    for f in fr: print('   ', f['label'], f['guard'], f['policy'], f['size'], 'black=', f['black_share'], 'masks=', f['masks'])
print(s['notes'], s.get('field_names'))
