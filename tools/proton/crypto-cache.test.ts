import { expect, test } from 'bun:test';
import { createCryptoCache } from './account';
test('request crypto cache retains keys until explicit invalidation', async () => {
    const cache = createCryptoCache(), value = {};
    await cache.setEntity('synthetic-node-key', value);
    expect(await cache.getEntity('synthetic-node-key')).toBe(value);
    await cache.removeEntities(['synthetic-node-key']);
    await expect(cache.getEntity('synthetic-node-key')).rejects.toThrow();
});
test('request crypto cache never crosses request instances', async () => {
    const first = createCryptoCache(), second = createCryptoCache();
    await first.setEntity('synthetic-node-key', {});
    await expect(second.getEntity('synthetic-node-key')).rejects.toThrow();
});
test('request crypto cache evicts least recently used entries at its fixed bound', async () => {
    const cache = createCryptoCache();
    for (let i = 0; i < 64; i++) await cache.setEntity(String(i), {});
    await cache.getEntity('0'); await cache.setEntity('64', {});
    await expect(cache.getEntity('1')).rejects.toThrow();
    expect(await cache.getEntity('0')).toEqual({});
    const found = [];
    for await (const value of cache.iterateEntities(Array.from({length: 65}, (_, i) => String(i)))) found.push(value);
    expect(found.filter(value => value.ok).length).toBe(64);
});
test('request crypto cache replacement, tagged enumeration and clearing preserve SDK semantics', async () => {
    const cache = createCryptoCache();
    await cache.setEntity('key', {}, ['old']); await cache.setEntity('key', {}, ['new']);
    const collect = async (tag: string) => { const entries = []; for await (const entry of cache.iterateEntitiesByTag(tag)) entries.push(entry); return entries; };
    expect(await collect('old')).toEqual([]); expect((await collect('new')).length).toBe(1);
    await cache.clear(); await expect(cache.getEntity('key')).rejects.toThrow();
    expect(await collect('new')).toEqual([]);
});
