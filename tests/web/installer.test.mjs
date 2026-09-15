import test from 'node:test';
import assert from 'node:assert/strict';
import {APP, HELP, VERSION, info, file, FakePort} from './fake-port.mjs';
import {FlipperSerial, parseDeviceInfo} from '../../web/serial.mjs';
import {compatibility, fetchFile, helpTarget} from '../../web/catalog.mjs';

test('firmware detection and compatibility do not guess custom firmware', () => {
  const device = parseDeviceInfo(info);
  assert.equal(device.api, '87.1');
  const build = {firmware: 'official', firmware_version: '1.4.3', api_version: '87.1'};
  assert.equal(compatibility(device, build).confirm, false);
  assert.equal(compatibility(device, {...build, firmware: 'momentum'}).blocked, true);
  assert.equal(compatibility({...device, api: '88.0'}, build).blocked, true);
  assert.equal(compatibility(device, {...build, firmware_version: null}).confirm, true);
  assert.equal(parseDeviceInfo(info.replace('flipperdevices', 'custom')).family, null);
  assert.equal(parseDeviceInfo(info.replace('1.4.3', 'mntm-012')).family, 'momentum');
  assert.equal(parseDeviceInfo(info.replace('1.4.3', 'unlshd-092')).family, 'unleashed');
  assert.throws(() => parseDeviceInfo('unrelated serial device'));
});

test('both files install correctly with one-byte response fragments and USB unlocks', async () => {
  const port = new FakePort({fragment: true});
  const client = new FlipperSerial(port);
  try {
    assert.equal((await client.open()).family, 'official');
    const files = [file(APP, new Uint8Array(19000).fill(128)), file(HELP, new Uint8Array([0, 13, 10, 62, 58, 32, 255]))];
    let bytes = 0;
    await client.install(files, (n) => { bytes += n; }, VERSION);
    assert.equal(bytes, 19007);
    for (const f of files) assert.deepEqual(port.files.get(f.target), f.data);
    assert.equal(port.files.size, 2);
    const firstRename = port.commands.findIndex((c) => c.startsWith('storage rename'));
    assert.ok(port.commands.indexOf(`storage md5 "${HELP}.web-install"`) < firstRename);
    assert.ok(port.commands.every((c) => !/save|settings|format/.test(c)));
  } finally { await client.close(); }
  assert.equal(port.closed, true);
});

for (const option of ['badHash', 'denyWrite', 'unplug', 'failCommit']) {
  test(`${option} preserves existing app and help`, async () => {
    const port = new FakePort({[option]: true});
    const client = new FlipperSerial(port, {timeout: 500});
    try {
      await client.open();
      await assert.rejects(client.install([file(APP, new Uint8Array([9, 8])), file(HELP, new Uint8Array([7, 6]))], undefined, VERSION));
      assert.deepEqual(port.files.get(APP), new Uint8Array([1, 2]));
      assert.deepEqual(port.files.get(HELP), new Uint8Array([3, 4]));
    } finally { await client.close(); }
  });
}

test('timed out command rejects and port is still released', async () => {
  const port = new FakePort({hang: true});
  const client = new FlipperSerial(port, {timeout: 20});
  await assert.rejects(client.open(), /timed out/);
  await client.close();
  assert.ok(port.closed);
});

test('retry replaces stale staging bytes; app-only release leaves help untouched', async () => {
  const port = new FakePort();
  port.files.set(APP + '.web-install', new Uint8Array([255, 255, 255]));
  const client = new FlipperSerial(port);
  try {
    await client.open();
    await client.install([file(APP, new Uint8Array([9]))]);
    assert.deepEqual(port.files.get(APP), new Uint8Array([9]));
    assert.deepEqual(port.files.get(HELP), new Uint8Array([3, 4]));
  } finally { await client.close(); }
});

test('existing recovery backup is never deleted or overwritten', async () => {
  const port = new FakePort();
  port.files.set(APP + '.web-backup', new Uint8Array([42]));
  const client = new FlipperSerial(port);
  try {
    await client.open();
    await assert.rejects(client.install([file(APP, new Uint8Array([9]))]), /interrupted installation/);
    assert.deepEqual(port.files.get(APP + '.web-backup'), new Uint8Array([42]));
    assert.deepEqual(port.files.get(APP), new Uint8Array([1, 2]));
  } finally { await client.close(); }
});

test('downloads are SHA-256 verified and resolve beneath the GitHub Pages project path', async () => {
  const data = new Uint8Array([0, 1, 255]);
  const record = {...file(APP, data), url: 'releases/123/app.fap'};
  let requested;
  const options = {base: 'https://example.github.io/Missile-CMD/', fetcher: async (url) => {
    requested = url.href;
    return new Response(data);
  }};
  assert.deepEqual((await fetchFile(record, options)).data, data);
  assert.equal(requested, 'https://example.github.io/Missile-CMD/releases/123/app.fap');
  await assert.rejects(fetchFile({...record, sha256: '0'.repeat(64)}, options), /checksum/);
  await assert.rejects(fetchFile({...record, size: 100}, options), /Incomplete/);
  await assert.rejects(fetchFile({...record, url: 'https://another.example/app.fap'}, options), /this site/);
});

test('versioned installation rejects another release folder and unsafe versions before writes', async () => {
  assert.equal(helpTarget(VERSION), HELP);
  for (const version of ['../1.2.3', 'v1.2.3', '1.2.3\n', '1.2.3\r', '01.2.3', '1.2.3-01', '1.2.3+bad..id']) {
    assert.throws(() => helpTarget(version), /Invalid release version/);
  }
  const port = new FakePort();
  const client = new FlipperSerial(port);
  try {
    await client.open();
    const before = port.commands.length;
    await assert.rejects(client.install([
      file(APP, new Uint8Array([1])), file('/ext/apps_data/missile_cmd/9.9.9/help.bin', new Uint8Array([2])),
    ], undefined, VERSION), /Invalid installation file set/);
    assert.equal(port.commands.length, before);
  } finally { await client.close(); }
});

const recoveryFiles = (port) => {
  const targets = [APP, HELP, '/ext/apps_data/missile_cmd/1.0.0/help.bin', '/ext/apps_data/missile_cmd/v9/help.bin'];
  return targets.map((target, index) => {
    port.dirs.add(target.slice(0, target.lastIndexOf('/')));
    const data = new Uint8Array([41 + index, 81 + index]);
    port.files.set(target, new Uint8Array([9, 9]));
    port.files.set(target + '.web-backup', data);
    return {target, data};
  });
};
const mutations = (commands) => commands.filter((c) => /^storage (remove|copy|rename|write_chunk|mkdir) /.test(c));

test('connection discovers app and all recognized help backups independently of selected release', async () => {
  const port = new FakePort({fragment: true});
  const originals = recoveryFiles(port);
  for (const folder of ['notes', '1.2.3"bad', '01.2.3', '1.2.3-01']) {
    port.dirs.add(`/ext/apps_data/missile_cmd/${folder}`);
    port.files.set(`/ext/apps_data/missile_cmd/${folder}/help.bin.web-backup`, new Uint8Array([77]));
  }
  const client = new FlipperSerial(port);
  try {
    await client.open();
    assert.deepEqual(client.recovery.map((f) => f.target).sort(), originals.map((f) => f.target).sort());
    assert.equal(mutations(port.commands).length, 0);
    await assert.rejects(client.install([file(APP, new Uint8Array([8]))]), /Restore backups/);
    assert.equal(mutations(port.commands).length, 0);
    for (const entry of originals) assert.deepEqual(port.files.get(entry.target + '.web-backup'), entry.data);
  } finally { await client.close(); }
});

test('a backup appearing after connection still blocks installation before writes', async () => {
  const port = new FakePort();
  const client = new FlipperSerial(port);
  try {
    await client.open();
    port.files.set(HELP + '.web-backup', new Uint8Array([7]));
    await assert.rejects(client.install([file(APP, new Uint8Array([8]))]), /Restore backups/);
    assert.equal(mutations(port.commands).length, 0);
  } finally { await client.close(); }
});

test('recovery stages and verifies every original before removing backups', async () => {
  const port = new FakePort({fragment: true});
  const originals = recoveryFiles(port);
  port.files.set(APP + '.web-restore', new Uint8Array([255]));
  const client = new FlipperSerial(port);
  try {
    await client.open();
    assert.equal((await client.restoreBackups()).status, 'restored');
    assert.equal(client.recovery.length, 0);
    for (const entry of originals) {
      assert.deepEqual(port.files.get(entry.target), entry.data);
      assert.ok(!port.files.has(entry.target + '.web-backup'));
      assert.ok(!port.files.has(entry.target + '.web-restore'));
    }
    const firstReplacement = port.commands.findIndex((c) => c === `storage remove "${APP}"`);
    const firstBackupRemoval = port.commands.findIndex((c) => c === `storage remove "${APP}.web-backup"`);
    for (const entry of originals) {
      assert.ok(port.commands.indexOf(`storage md5 "${entry.target}.web-restore"`) < firstReplacement);
      assert.ok(port.commands.indexOf(`storage md5 "${entry.target}"`) < firstBackupRemoval);
    }
    assert.ok(!port.commands.some((c) => c.includes('write_chunk')));
  } finally { await client.close(); }
  assert.ok(port.closed);
});

test('every interrupted recovery step keeps originals available for a reconnect and retry', async () => {
  let completed = false;
  for (let fault = 0; fault < 160 && !completed; fault++) {
    const port = new FakePort();
    const originals = recoveryFiles(port);
    let client = new FlipperSerial(port);
    await client.open();
    let calls = 0;
    let fired = false;
    port.options.fault = () => {
      if (calls++ !== fault) return false;
      fired = true;
      return true;
    };
    let failed = false;
    try { await client.restoreBackups(); } catch { failed = true; }
    assert.equal(failed, fired);
    completed = !fired;
    port.options.fault = null;
    for (const entry of originals) {
      assert.deepEqual(port.files.get(entry.target + '.web-backup') || port.files.get(entry.target), entry.data);
    }
    await client.close();
    client = new FlipperSerial(port);
    try {
      await client.open();
      if (client.recovery.length) await client.restoreBackups();
      for (const entry of originals) assert.deepEqual(port.files.get(entry.target), entry.data);
      assert.equal(client.recovery.length, 0);
    } finally { await client.close(); }
    assert.ok(port.closed);
  }
  assert.ok(completed, 'fault sweep reached a successful full recovery');
});

test('a corrupted recovery copy never replaces installed files or consumes backups', async () => {
  const port = new FakePort({corruptCopy: true});
  const originals = recoveryFiles(port);
  const client = new FlipperSerial(port);
  try {
    await client.open();
    await assert.rejects(client.restoreBackups(), /checksum/);
    for (const entry of originals) {
      assert.deepEqual(port.files.get(entry.target), new Uint8Array([9, 9]));
      assert.deepEqual(port.files.get(entry.target + '.web-backup'), entry.data);
    }
  } finally { await client.close(); }
});

test('changed backups require a fresh review before recovery writes', async () => {
  const port = new FakePort();
  recoveryFiles(port);
  const client = new FlipperSerial(port);
  try {
    await client.open();
    port.files.set(APP + '.web-backup', new Uint8Array([7, 7]));
    await assert.rejects(client.restoreBackups(), /changed/);
    assert.equal(mutations(port.commands).length, 0);
  } finally { await client.close(); }
});

test('verified installation reports cleanup warnings without rolling back installed files', async () => {
  const port = new FakePort({fault: ({operation, path}) => operation === 'remove' && path.endsWith('.web-backup')});
  const client = new FlipperSerial(port);
  try {
    await client.open();
    const files = [file(APP, new Uint8Array([8, 7])), file(HELP, new Uint8Array([6, 5]))];
    const result = await client.install(files, undefined, VERSION);
    assert.equal(result.status, 'installed-with-cleanup-warnings');
    assert.equal(result.cleanupWarnings.length, 2);
    for (const f of files) assert.deepEqual(port.files.get(f.target), f.data);
    assert.deepEqual(port.files.get(APP + '.web-backup'), new Uint8Array([1, 2]));
    assert.deepEqual(port.files.get(HELP + '.web-backup'), new Uint8Array([3, 4]));
  } finally { await client.close(); }
});

test('USB loss after recovery writes and checks releases locks and remains retryable', async () => {
  const interruptions = [
    ({operation}) => operation === 'copy',
    ({operation, path}) => operation === 'remove' && path === APP,
    ({operation}) => operation === 'rename',
    ({operation, path}) => operation === 'md5' && path === APP,
    ({operation, path}) => operation === 'remove' && path.endsWith('.web-backup'),
  ];
  for (const unplugAfter of interruptions) {
    const port = new FakePort();
    const originals = recoveryFiles(port);
    let client = new FlipperSerial(port);
    await client.open();
    port.options.unplugAfter = unplugAfter;
    await assert.rejects(client.restoreBackups(), /USB removed/);
    await client.close();
    assert.ok(port.closed);
    for (const entry of originals) {
      assert.deepEqual(port.files.get(entry.target + '.web-backup') || port.files.get(entry.target), entry.data);
    }
    port.options.unplugAfter = null;
    client = new FlipperSerial(port);
    try {
      await client.open();
      await client.restoreBackups();
      for (const entry of originals) assert.deepEqual(port.files.get(entry.target), entry.data);
    } finally { await client.close(); }
  }
});

test('an unreadable backup blocks connection without attempting writes or directory checksums', async () => {
  const port = new FakePort();
  port.dirs.add(APP + '.web-backup');
  const client = new FlipperSerial(port);
  try {
    await assert.rejects(client.open(), /Cannot verify the backup/);
    assert.equal(client.recoveryChecked, false);
    assert.equal(mutations(port.commands).length, 0);
    assert.equal(port.commands.some((cmd) => cmd.startsWith('storage md5')), false);
  } finally { await client.close(); }
});
