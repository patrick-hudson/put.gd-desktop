import { NullCache, type EntityResult } from '@protontech/drive-sdk/cache';

// Decrypted keys are bounded in memory for one helper request. Entity metadata
// uses NullCache independently, so ownership/revision reads still reach the API.
export class RequestCryptoCache<T> extends NullCache<T> {
    private readonly entries = new Map<string, { value: T; tags: readonly string[] }>();
    override async clear(): Promise<void> { this.entries.clear(); }
    override async setEntity(key: string, value: T, tags: string[] = []): Promise<void> {
        this.entries.delete(key);
        this.entries.set(key, { value, tags: [...tags] });
        if (this.entries.size > 64) this.entries.delete(this.entries.keys().next().value!);
    }
    override async getEntity(key: string): Promise<T> {
        const entry = this.entries.get(key);
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
        for (const [key, entry] of [...this.entries])
            if (entry.tags.includes(tag)) yield { key, ok: true, value: entry.value };
    }
}
