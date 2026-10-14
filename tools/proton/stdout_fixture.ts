// Separately compiled regression executable, never installed as the production runtime.
import { writeJson } from './stdout';
await writeJson({ synthetic: 'x'.repeat(1024 * 1024 - 256) });
process.exit(0);
