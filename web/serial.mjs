// Flipper's public storage CLI protocol, also used by its SDK uploader:
// https://github.com/flipperdevices/flipperzero-firmware/blob/1.4.3/scripts/flipper/storage.py
import { helpTarget } from './catalog.mjs';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const PROMPT = '>: ';
const APP_TARGET = '/ext/apps/Games/missile_cmd.fap';
const DATA_ROOT = '/ext/apps_data/missile_cmd';

function versionFolder(name) {
    if (/^v[0-9]+$/.test(name)) return true;
    try { helpTarget(name); return true; } catch { return false; }
}

function checkResponse(text, allowed = []) {
    const error = /Storage error:\s*([^\r\n]+)/i.exec(text);
    if (error && !allowed.includes(error[1].trim())) throw new Error(`Flipper storage: ${error[1].trim()}.`);
    if (/command not found|unknown command|Usage:|another application is running/i.test(text)) {
        throw new Error('The Flipper could not run the storage command. Close the game and reconnect.');
    }
    return text;
}

export function parseDeviceInfo(text) {
    const fields = Object.fromEntries([...text.matchAll(/^([\w]+)\s*:\s*([^\r\n]*)/gm)]
        .map((match) => [match[1], match[2].trim()]));
    if (!fields.hardware_model || !fields.firmware_version) {
        throw new Error('This port did not identify as a Flipper Zero. Close qFlipper and reconnect.');
    }
    if (fields.hardware_model !== 'Flipper Zero' || (fields.hardware_target && fields.hardware_target !== '7')) {
        throw new Error('This installer supports Flipper Zero (f7) only.');
    }
    const origin = fields.firmware_origin_fork || fields.firmware_origin || '';
    const identity = [origin, fields.firmware_origin_git, fields.firmware_version, fields.firmware_branch].filter(Boolean).join(' ');
    const family = /momentum|mntm/i.test(identity) ? 'momentum'
        : /unleashed|unlshd/i.test(identity) ? 'unleashed'
            : /^(flipperdevices|flipper devices|official)$/i.test(origin) ? 'official' : null;
    const api = /^\d+$/.test(fields.firmware_api_major || '') && /^\d+$/.test(fields.firmware_api_minor || '')
        ? `${fields.firmware_api_major}.${fields.firmware_api_minor}` : fields.firmware_api || null;
    return { family, version: fields.firmware_version, api: api === '0.0' ? null : api };
}

export class FlipperSerial {
    constructor(port, { timeout = 15000, onDisconnect = () => { } } = {}) {
        this.port = port;
        this.timeout = timeout;
        this.onDisconnect = onDisconnect;
        this.buffer = '';
        this.waiter = null;
        this.failure = null;
        this.closing = false;
        this.recovery = [];
        this.recoveryChecked = false;
    }

    async open() {
        await this.port.open({ baudRate: 115200, bufferSize: 16384 });
        this.reader = this.port.readable.getReader();
        this.writer = this.port.writable.getWriter();
        this.reading = this.readLoop();
        // Consume the banner/prompt before sending commands. DTR is needed by USB CDC.
        await this.port.setSignals({ dataTerminalReady: true });
        await this.until(PROMPT);
        const info = await this.command('device_info');
        this.device = parseDeviceInfo(info);
        // Missing/unusable SD cards fail before any write.
        const storage = await this.command('storage stat "/ext"');
        if (!/Storage,/.test(storage)) throw new Error('Insert a working microSD card into your Flipper.');
        await this.inspectRecovery();
        return this.device;
    }

    async readLoop() {
        try {
            while (!this.closing) {
                const { value, done } = await this.reader.read();
                if (done) break;
                this.buffer += decoder.decode(value);
                if (this.buffer.length > 131072) throw new Error('Unexpected response from the Flipper. Reconnect and retry.');
                this.flush();
            }
            if (!this.closing) this.fail(new Error('Flipper disconnected. Reconnect the USB cable and retry.'));
        } catch (error) {
            if (!this.closing) this.fail(error);
        } finally {
            this.reader.releaseLock();
        }
    }

    fail(error) {
        if (this.failure) return;
        this.failure = error;
        if (this.waiter) {
            clearTimeout(this.waiter.timer);
            this.waiter.reject(error);
            this.waiter = null;
        }
        this.onDisconnect(error);
    }

    flush() {
        if (!this.waiter) return;
        const index = this.buffer.indexOf(this.waiter.marker);
        if (index < 0) return;
        const text = this.buffer.slice(0, index);
        this.buffer = this.buffer.slice(index + this.waiter.marker.length);
        clearTimeout(this.waiter.timer);
        this.waiter.resolve(text);
        this.waiter = null;
    }

    until(marker) {
        if (this.failure) return Promise.reject(this.failure);
        if (this.waiter) return Promise.reject(new Error('A serial command is already in progress.'));
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => this.fail(new Error('Flipper response timed out. Close other Flipper apps, reconnect, and retry.')), this.timeout);
            this.waiter = { marker, resolve, reject, timer };
            this.flush();
        });
    }

    async write(data) {
        if (this.failure) throw this.failure;
        let timer;
        try {
            await Promise.race([
                this.writer.write(typeof data === 'string' ? encoder.encode(data) : data),
                new Promise((_, reject) => {
                    timer = setTimeout(() => reject(new Error('USB write timed out. Reconnect your Flipper.')), this.timeout);
                }),
            ]);
        } catch (error) {
            this.fail(error);
            throw error;
        } finally {
            clearTimeout(timer);
        }
    }

    async command(text, allowed = []) {
        await this.write(text + '\r');
        return checkResponse(await this.until(PROMPT), allowed);
    }

    async exists(path) {
        const response = await this.command(`storage stat "${path}"`, ['file/dir not exist']);
        return !response.includes('Storage error:');
    }

    async remove(path) {
        await this.command(`storage remove "${path}"`, ['file/dir not exist']);
    }

    async rename(from, to) {
        await this.command(`storage rename "${from}" "${to}"`);
    }

    async fingerprint(path, name) {
        const stat = await this.command(`storage stat "${path}"`);
        const size = Number(/(?:^|\n)File, size:\s*(\d+)b\r?(?:\n|$)/.exec(stat)?.[1]);
        if (!Number.isSafeInteger(size) || size < 0) {
            throw new Error(`Cannot verify the backup for ${name}. Check the microSD card and reconnect.`);
        }
        const response = await this.command(`storage md5 "${path}"`);
        const md5 = response.split(/\r?\n/).find((line) => /^[a-f0-9]{32}$/i.test(line.trim()))?.trim().toLowerCase();
        if (!md5) {
            throw new Error(`Cannot verify the backup for ${name}. Check the microSD card and reconnect.`);
        }
        return { name, size, md5 };
    }

    async inspectRecovery() {
        this.recoveryChecked = false;
        const candidates = [{ target: APP_TARGET, name: 'Missile CMD app' }];
        const listing = await this.command(`storage list "${DATA_ROOT}"`, ['file/dir not exist']);
        for (const match of listing.matchAll(/^\s*\[D\] ([^\r\n]+)\r?$/gm)) {
            const version = match[1];
            if (versionFolder(version)) candidates.push({ target: `${DATA_ROOT}/${version}/help.bin`, name: `Help — ${version}` });
        }
        const found = [];
        for (const entry of candidates) {
            const backup = entry.target + '.web-backup';
            if (await this.exists(backup)) {
                found.push({ ...entry, backup, staged: entry.target + '.web-restore',
                    ...await this.fingerprint(backup, entry.name) });
            }
        }
        this.recovery = found;
        this.recoveryChecked = true;
        return found;
    }

    async restoreBackups(progress = () => { }) {
        const approved = this.recovery;
        const entries = await this.inspectRecovery();
        const same = (a, b) => a.target === b.target && a.size === b.size && a.md5 === b.md5;
        if (!approved.length || entries.length !== approved.length ||
            entries.some((entry) => !approved.some((prior) => same(entry, prior)))) {
            throw new Error('Backup files changed. Reconnect and review the recovery list again.');
        }
        // Stage every original before touching installed files. Backups remain available
        // through copy, replacement, verification, and any interrupted retry.
        for (const entry of entries) {
            progress(`Restoring ${entry.name}…`);
            await this.remove(entry.staged);
            await this.command(`storage copy "${entry.backup}" "${entry.staged}"`);
            await this.verify(entry.staged, entry);
        }
        for (const entry of entries) {
            await this.remove(entry.target);
            await this.rename(entry.staged, entry.target);
        }
        for (const entry of entries) await this.verify(entry.target, entry);
        for (const entry of entries) await this.remove(entry.backup);
        await this.inspectRecovery();
        if (this.recovery.length) throw new Error('More backups were found. Reconnect and review them before installing.');
        return { status: 'restored' };
    }

    async verify(path, file) {
        const stat = await this.command(`storage stat "${path}"`);
        if (Number(/File, size:\s*(\d+)b/.exec(stat)?.[1]) !== file.size) {
            throw new Error(`File size check failed for ${file.name}. Reconnect and retry.`);
        }
        const hash = await this.command(`storage md5 "${path}"`);
        if (!hash.split(/\r?\n/).some((line) => line.trim().toLowerCase() === file.md5)) {
            throw new Error(`File checksum check failed for ${file.name}. Reconnect and retry.`);
        }
    }

    async upload(path, file, progress) {
        await this.remove(path); // write_chunk appends; a retry must start with an empty file.
        for (let offset = 0; offset < file.data.length; offset += 8192) {
            const chunk = file.data.subarray(offset, offset + 8192);
            await this.write(`storage write_chunk "${path}" ${chunk.length}\r`);
            // Echo and Ready can arrive split or coalesced; do not send bytes before Ready.
            let ready = false;
            while (!ready) {
                const line = checkResponse(await this.until('\r\n'));
                ready = line.trim() === 'Ready';
            }
            await this.write(chunk);
            checkResponse(await this.until(PROMPT));
            progress(chunk.length);
        }
        await this.verify(path, file);
    }

    async install(files, progress = () => { }, version = null) {
        const targets = version === null ? [APP_TARGET] : [APP_TARGET, helpTarget(version)];
        if (files.length !== targets.length || files[0].target !== APP_TARGET ||
            new Set(files.map((f) => f.target)).size !== files.length ||
            files.some((f) => !targets.includes(f.target) || !f.data?.length ||
                f.data.length !== f.size || !/^[a-f0-9]{32}$/.test(f.md5))) {
            throw new Error('Invalid installation file set.');
        }
        // Check all release folders again before writes, even for an app-only install.
        if ((await this.inspectRecovery()).length) {
            throw new Error('An interrupted installation needs recovery. Reconnect and choose Restore backups before installing.');
        }
        const directories = new Set();
        for (const file of files) {
            const parts = file.target.split('/').slice(2, -1);
            let directory = '/ext';
            for (const part of parts) {
                directory += '/' + part;
                directories.add(directory);
            }
        }
        for (const directory of directories) {
            await this.command(`storage mkdir "${directory}"`, ['file/dir already exist']);
        }
        const entries = files.map((file) => ({
            file, staged: file.target + '.web-install',
            backup: file.target + '.web-backup', backedUp: false, installed: false
        }));
        // Both downloads are already SHA-256 verified. Stage AND verify both before
        // touching either installed file. Saves/settings are never addressed.
        for (const entry of entries) {
            progress(0, `Copying ${entry.file.name}…`);
            await this.upload(entry.staged, entry.file, (bytes) => progress(bytes));
        }
        progress(0, 'Finishing and verifying installation…');
        try {
            for (const entry of entries) {
                if (await this.exists(entry.file.target)) {
                    await this.rename(entry.file.target, entry.backup);
                    entry.backedUp = true;
                }
                await this.rename(entry.staged, entry.file.target);
                entry.installed = true;
            }
            for (const entry of entries) await this.verify(entry.file.target, entry.file);
        } catch (error) {
            if (!this.failure) {
                try {
                    for (const entry of [...entries].reverse()) {
                        if (entry.installed) await this.remove(entry.file.target);
                        if (entry.backedUp) await this.rename(entry.backup, entry.file.target);
                    }
                } catch {
                    throw new Error('Installation interrupted. Reconnect and choose Restore backups to recover the original files.');
                }
            }
            throw new Error(`${error.message} Reconnect to check for backups before retrying.`);
        }
        const cleanupWarnings = [];
        for (const entry of entries) {
            if (entry.backedUp) {
                try { await this.remove(entry.backup); }
                catch { cleanupWarnings.push(entry.file.name); }
            }
        }
        return { status: cleanupWarnings.length ? 'installed-with-cleanup-warnings' : 'installed', cleanupWarnings };
    }

    async close() {
        if (this.closing) return;
        this.closing = true;
        if (this.waiter) {
            clearTimeout(this.waiter.timer);
            this.waiter.reject(new Error('Connection closed.'));
            this.waiter = null;
        }
        // Cancel reads before closing the port: Web Serial requires unlocked streams.
        try { await this.reader?.cancel(); } catch { /* Already unplugged. */ }
        await this.reading;
        try { await this.writer?.abort(); } catch { /* Already unplugged. */ }
        try { this.writer?.releaseLock(); } catch { /* Already released. */ }
        try { await this.port.setSignals({ dataTerminalReady: false }); } catch { /* Port gone. */ }
        try { await this.port.close(); } catch { /* Port already closed. */ }
    }
}
