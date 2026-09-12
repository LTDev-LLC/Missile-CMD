import test from 'node:test';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {FlipperSerial, parseDeviceInfo} from '../../web/serial.mjs';
import {compatibility, fetchFile, helpTarget} from '../../web/catalog.mjs';

const APP = '/ext/apps/Games/missile_cmd.fap';
const VERSION = '1.2.3-rc.1+usb';
const HELP = `/ext/apps_data/missile_cmd/${VERSION}/help.bin`;
const info = 'hardware_model : Flipper Zero\r\nhardware_target : 7\r\nfirmware_version : 1.4.3\r\nfirmware_origin_fork : flipperdevices\r\nfirmware_api_major : 87\r\nfirmware_api_minor : 1\r\n';
const hash = (type, bytes) => createHash(type).update(bytes).digest('hex');
const file = (target, data) => ({name: target.split('/').at(-1), target, data,
  size: data.length, md5: hash('md5', data), sha256: hash('sha256', data)});

// Real Web Streams with command echoes, binary writes, and arbitrarily split
// device responses. This exercises the transport, not a mock of its methods.
class FakePort {
  constructor(options = {}) {
    this.options = options;
    this.files = new Map([[APP, new Uint8Array([1, 2])], [HELP, new Uint8Array([3, 4])]]);
    this.dirs = new Set(['/ext', '/ext/apps', '/ext/apps/Games', '/ext/apps_data', '/ext/apps_data/missile_cmd']);
    this.commands = [];
    this.binary = null;
    this.closed = false;
  }
  async open() {
    this.readable = new ReadableStream({start: (controller) => { this.output = controller; }});
    this.writable = new WritableStream({write: (bytes) => this.receive(bytes)});
  }
  async setSignals({dataTerminalReady}) {
    if (dataTerminalReady) this.respond('Flipper Zero\r\n>: ');
  }
  async close() {
    assert.equal(this.readable.locked, false);
    assert.equal(this.writable.locked, false);
    this.closed = true;
  }
  respond(text) {
    const bytes = new TextEncoder().encode(text);
    const step = this.options.fragment ? 1 : 37;
    for (let i = 0; i < bytes.length; i += step) this.output.enqueue(bytes.slice(i, i + step));
  }
  receive(bytes) {
    if (this.binary) {
      assert.equal(bytes.length, this.binary.size);
      const target = this.binary.path;
      if (this.options.unplug) { this.output.error(new Error('USB removed')); return; }
      const old = this.files.get(target) || new Uint8Array();
      this.files.set(target, new Uint8Array([...old, ...bytes]));
      this.binary = null;
      this.respond('\r\n>: ');
      return;
    }
    const command = new TextDecoder().decode(bytes).replace(/\r$/, '');
    this.commands.push(command);
    this.respond(command + '\r\n');
    if (this.options.hang && command === 'device_info') return;
    const paths = [...command.matchAll(/"([^"]+)"/g)].map((m) => m[1]);
    const [path, destination] = paths;
    const operation = command.split(' ')[1];
    let response = '';
    if (command === 'device_info') response = this.options.info || info;
    else if (operation === 'stat') {
      response = path === '/ext' ? 'Storage, 10000KiB total, 9000KiB free'
        : this.files.has(path) ? `File, size: ${this.files.get(path).length}b`
        : this.dirs.has(path) ? 'Directory' : 'Storage error: file/dir not exist';
    } else if (operation === 'mkdir') {
      if (this.dirs.has(path)) response = 'Storage error: file/dir already exist';
      else this.dirs.add(path);
    } else if (operation === 'remove') this.files.delete(path);
    else if (operation === 'write_chunk') {
      assert.ok(this.dirs.has(path.slice(0, path.lastIndexOf('/'))), 'create the version directory before uploading');
      if (this.options.denyWrite) response = 'Storage error: access denied';
      else {
        this.binary = {path, size: Number(command.split(' ').at(-1))};
        this.respond('Ready\r\n');
        return;
      }
    } else if (operation === 'md5') {
      response = this.options.badHash && path.startsWith(HELP) ? '0'.repeat(32) : hash('md5', this.files.get(path));
    } else if (operation === 'rename') {
      if (this.options.failCommit && path === HELP + '.web-install') response = 'Storage error: access denied';
      else {
        assert.ok(this.files.has(path));
        assert.ok(!this.files.has(destination), 'installer must back up before replacing');
        this.files.set(destination, this.files.get(path));
        this.files.delete(path);
      }
    } else throw new Error('Unexpected command: ' + command);
    this.respond(response + '\r\n>: ');
  }
}

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
