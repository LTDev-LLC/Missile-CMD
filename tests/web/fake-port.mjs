import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';

export const APP = '/ext/apps/Games/missile_cmd.fap';
export const VERSION = '1.2.3-rc.1+usb';
export const HELP = `/ext/apps_data/missile_cmd/${VERSION}/help.bin`;
export const info = 'hardware_model : Flipper Zero\r\nhardware_target : 7\r\nfirmware_version : 1.4.3\r\nfirmware_origin_fork : flipperdevices\r\nfirmware_api_major : 87\r\nfirmware_api_minor : 1\r\n';
export const hash = (type, bytes) => createHash(type).update(bytes).digest('hex');
export const file = (target, data) => ({name: target.split('/').at(-1), target, data,
  size: data.length, md5: hash('md5', data), sha256: hash('sha256', data)});

// Real Web Streams with command echoes, binary writes, and arbitrarily split
// device responses. This exercises the transport, not a mock of its methods.
export class FakePort {
  constructor(options = {}) {
    this.options = options;
    this.files = new Map([[APP, new Uint8Array([1, 2])], [HELP, new Uint8Array([3, 4])]]);
    this.dirs = new Set(['/ext', '/ext/apps', '/ext/apps/Games', '/ext/apps_data', '/ext/apps_data/missile_cmd']);
    this.dirs.add(HELP.slice(0, HELP.lastIndexOf('/')));
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
    if (this.options.fault?.({operation, path, destination, port: this})) {
      this.respond('Storage error: access denied\r\n>: ');
      return;
    }
    let response = '';
    if (command === 'device_info') response = this.options.info || info;
    else if (operation === 'stat') {
      response = path === '/ext' ? 'Storage, 10000KiB total, 9000KiB free'
        : this.files.has(path) ? `File, size: ${this.files.get(path).length}b`
        : this.dirs.has(path) ? 'Directory' : 'Storage error: file/dir not exist';
    } else if (operation === 'list') {
      if (!this.dirs.has(path)) response = 'Storage error: file/dir not exist';
      else {
        const direct = (name) => name.startsWith(path + '/') && !name.slice(path.length + 1).includes('/');
        const folders = [...this.dirs].filter(direct).map((name) => `\t[D] ${name.slice(path.length + 1)}`);
        const files = [...this.files].filter(([name]) => direct(name)).map(([name, data]) => `\t[F] ${name.slice(path.length + 1)} ${data.length}b`);
        response = [...folders, ...files].join('\r\n') || '\tEmpty';
      }
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
    } else if (operation === 'copy') {
      assert.ok(this.files.has(path));
      assert.ok(!this.files.has(destination), 'recovery staging must be reset before copying');
      this.files.set(destination, this.files.get(path).slice());
      if (this.options.corruptCopy) this.files.get(destination)[0] ^= 1;
    } else if (operation === 'rename') {
      if (this.options.failCommit && path === HELP + '.web-install') response = 'Storage error: access denied';
      else {
        assert.ok(this.files.has(path));
        assert.ok(!this.files.has(destination), 'installer must back up before replacing');
        this.files.set(destination, this.files.get(path));
        this.files.delete(path);
      }
    } else throw new Error('Unexpected command: ' + command);
    if (this.options.unplugAfter?.({operation, path, destination})) {
      this.output.error(new Error('USB removed'));
      return;
    }
    this.respond(response + '\r\n>: ');
  }
}
