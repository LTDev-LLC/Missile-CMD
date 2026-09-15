import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { APP, HELP, VERSION, FakePort, file, info } from './fake-port.mjs';

// Exercise the real event handlers, catalog checks, downloads, and serial client.
// Only browser elements and the physical USB device are replaced.
class Element {
  constructor(tag = '') {
    this.tag = tag;
    this.listeners = {};
    this.children = [];
    this.hidden = false;
    this.disabled = false;
    this.checked = false;
    this.value = '';
    this.textContent = '';
  }
  addEventListener(type, handler) { this.listeners[type] = handler; }
  replaceChildren(...children) {
    this.children = children;
    if (this.tag === 'select') this.value = children[0]?.value ?? '';
  }
  append(...children) { this.children.push(...children); }
  async dispatch(type) {
    if (type === 'click' && this.disabled) return;
    await this.listeners[type]?.({preventDefault() {}});
  }
}

let fixtureId = 0;
async function page(t, port) {
  const html = await readFile(new URL('../../web/index.html', import.meta.url), 'utf8');
  const nodes = new Map([...html.matchAll(/<(\w+)[^>]*\bid="([^"]+)"[^>]*>/g)]
    .map(([, tag, id]) => [id, new Element(tag)]));
  const app = file(APP, new Uint8Array([11, 12, 13]));
  const help = file(HELP, new Uint8Array([21, 22, 23]));
  const files = [app, help].map((entry, i) => ({...entry, url: `./asset-${i}`}));
  const build = {firmware: 'official', firmware_version: '1.4.3', api_version: '87.1', data_version: VERSION, files};
  const releases = [
    {tag: 'v1.3.0', builds: [build, {...build, firmware: 'momentum', api_version: '88.0'}]},
    {tag: 'v1.0.0', builds: [{...build, firmware_version: null, data_version: null, files: [files[0]]}]},
  ];
  const fetches = [];
  const globals = {
    document: {getElementById: (id) => {assert.ok(nodes.has(id), id); return nodes.get(id);}, createElement: (tag) => new Element(tag)},
    window: {isSecureContext: true, addEventListener() {}},
    navigator: {serial: {requestPort: async () => port}},
    location: {href: 'https://installer.test/'},
    Option: class extends Element { constructor(text, value = text) { super('option'); this.textContent = text; this.value = value; } },
    fetch: async (url) => {
      fetches.push(String(url));
      if (url === './catalog.json') return {ok: true, json: async () => ({schema: 1, repository: 'owner/repo', releases})};
      const entry = files.find((f) => new URL(f.url, 'https://installer.test/').href === String(url));
      assert.ok(entry, String(url));
      return {ok: true, arrayBuffer: async () => entry.data.buffer};
    },
  };
  for (const [name, value] of Object.entries(globals)) {
    const previous = Object.getOwnPropertyDescriptor(globalThis, name);
    Object.defineProperty(globalThis, name, {configurable: true, writable: true, value});
    t.after(() => previous ? Object.defineProperty(globalThis, name, previous) : delete globalThis[name]);
  }
  await import(`../../web/app.mjs?fixture=${++fixtureId}`);
  return {get: (id) => nodes.get(id), fetches, files};
}

test('recovery survives release/firmware changes, releases USB, and requires a fresh install', async (t) => {
  const port = new FakePort({fragment: true});
  const original = new Uint8Array([71, 72]);
  port.files.set(APP + '.web-backup', original);
  const {get, fetches, files} = await page(t, port);
  await get('connect').dispatch('click');
  assert.equal(get('recovery').hidden, false);
  assert.deepEqual(get('recovery-files').children.map((item) => item.textContent), ['Missile CMD app']);
  assert.equal(get('install').disabled, true);
  get('firmware').value = 'momentum';
  await get('firmware').dispatch('change');
  assert.match(get('compatibility').textContent, /uses Official/);
  assert.equal(get('restore').disabled, false);
  get('release').value = 'v1.0.0';
  await get('release').dispatch('change');
  get('confirm').checked = true;
  await get('confirm').dispatch('change');
  assert.equal(get('install').disabled, true);
  await get('install').dispatch('click');
  assert.equal(fetches.length, 1);
  await get('restore').dispatch('click');
  assert.match(get('status').textContent, /restored and verified.*Reconnect/);
  assert.equal(port.closed, true);
  assert.equal(get('install').disabled, true);
  assert.equal(get('confirm').checked, false);
  assert.deepEqual(port.files.get(APP), original);
  assert.equal(port.commands.some((cmd) => cmd.startsWith('storage write_chunk')), false);
  // Reconnection still requires compatibility confirmation for this older build.
  await get('connect').dispatch('click');
  assert.equal(get('recovery').hidden, true);
  assert.equal(get('install').disabled, true);
  get('confirm').checked = true;
  await get('confirm').dispatch('change');
  assert.equal(get('install').disabled, false);
  assert.deepEqual(port.files.get(APP), original);
  await get('install').dispatch('click');
  assert.deepEqual(port.files.get(APP), files[0].data);
  assert.match(get('status').textContent, /installed and verified/);
  assert.equal(get('progress').value, 100);
  assert.equal(port.closed, true);
});

test('disconnecting from recovery leaves backups untouched', async (t) => {
  const port = new FakePort();
  port.files.set(HELP + '.web-backup', new Uint8Array([42]));
  const {get} = await page(t, port);
  await get('connect').dispatch('click');
  assert.equal(get('recovery').hidden, false);
  await get('disconnect').dispatch('click');
  assert.equal(port.closed, true);
  assert.ok(port.files.has(HELP + '.web-backup'));
  assert.equal(port.commands.some((cmd) => /storage (remove|copy|rename|write_chunk)/.test(cmd)), false);
});

test('failed restoration releases USB and preserves retryable backups', async (t) => {
  const port = new FakePort({fault: ({operation}) => operation === 'copy'});
  port.files.set(APP + '.web-backup', new Uint8Array([42]));
  const {get} = await page(t, port);
  await get('connect').dispatch('click');
  await get('restore').dispatch('click');
  assert.match(get('status').textContent, /Reconnect to retry recovery/);
  assert.equal(port.closed, true);
  assert.equal(get('install').disabled, true);
  assert.ok(port.files.has(APP + '.web-backup'));
});

test('a verified installation with leftover backups shows success with a cleanup warning', async (t) => {
  const port = new FakePort({fault: ({operation, path}) => operation === 'remove' && path.endsWith('.web-backup')});
  const {get, files} = await page(t, port);
  await get('connect').dispatch('click');
  await get('install').dispatch('click');
  assert.match(get('status').textContent, /installed and verified.*backups could not be removed/);
  assert.equal(get('progress').value, 100);
  assert.doesNotMatch(get('status').className, /error/);
  assert.deepEqual(port.files.get(APP), files[0].data);
  assert.equal(port.closed, true);
});

test('normal connections retain firmware and API mismatch blocks', async (t) => {
  const port = new FakePort({info: info.replace('firmware_api_major : 87', 'firmware_api_major : 86')});
  const {get} = await page(t, port);
  await get('connect').dispatch('click');
  assert.equal(get('recovery').hidden, true);
  assert.match(get('compatibility').textContent, /API 86.1 does not match/);
  assert.equal(get('install').disabled, true);
  await get('disconnect').dispatch('click');
});
