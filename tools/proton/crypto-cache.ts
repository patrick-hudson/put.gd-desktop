import { NullCache, type EntityResult } from '@protontech/drive-sdk/cache';

// One helper request's bounded memory: an instance for decrypted keys and one
// for node metadata. Nothing is written to disk or outlives the process.
export class RequestCache<T> extends NullCache<T> {
    private readonly entries = new Map<string, { value: T; tags: readonly string[] }>();
    private answering = true;
    // Runs a read the cache must not answer: every lookup misses meanwhile, so the SDK
    // fetches from the service. What it fetched is stored and answers later lookups.
    async fresh<R>(read: () => Promise<R>): Promise<R> {
        this.answering = false;
        try { return await read(); } finally { this.answering = true; }
    }
    override async clear(): Promise<void> { this.entries.clear(); }
    override async setEntity(key: string, value: T, tags: string[] = []): Promise<void> {
        this.entries.delete(key);
        this.entries.set(key, { value, tags: [...tags] });
        if (this.entries.size > 64) this.entries.delete(this.entries.keys().next().value!);
    }
    override async getEntity(key: string): Promise<T> {
        const entry = this.answering ? this.entries.get(key) : undefined;
        if (!entry) throw new Error('Entity not found');
        this.entries.delete(key); this.entries.set(key, entry);
        return entry.value;
    }
    override async removeEntities(keys: string[]): Promise<void> {
        for (const key of keys) this.entries.delete(key);
    }
    override async *iterateEntities(keys: string[]): AsyncGenerator<EntityResult<T>> {
        for (const key of keys) {
            try { yield { key, ok: true, value: await this.getEntity(key) }; }
            catch { yield { key, ok: false, error: 'Entity not found' }; }
        }
    }
    override async *iterateEntitiesByTag(tag: string): AsyncGenerator<EntityResult<T>> {
        for (const [key, entry] of this.answering ? [...this.entries] : [])
            if (entry.tags.includes(tag)) yield { key, ok: true, value: entry.value };
    }
}
