import { FlipperSerial } from './serial.mjs';
import { compatibility, fetchFile, FIRMWARE_NAMES } from './catalog.mjs';

const $ = (id) => document.getElementById(id);
let releases = [];
let connection = null;
let device = null;
let busy = false;
const supported = 'serial' in navigator && window.isSecureContext;
const selectedRelease = () => releases.find((release) => release.tag === $('release').value);
const selectedBuild = () => selectedRelease()?.builds.find((build) => build.firmware === $('firmware').value);

function status(message, kind = '') {
    $('status').textContent = message;
    $('status').className = `status ${kind}`;
}

function refresh() {
    const build = selectedBuild();
    const check = compatibility(device, build);
    $('release').disabled = busy || !releases.length;
    $('firmware').disabled = busy || !selectedRelease();
    $('connect').disabled = busy || !supported || Boolean(connection) || !build;
    $('disconnect').hidden = !connection;
    $('disconnect').disabled = busy;
    $('compatibility').textContent = check.message;
    $('confirm-row').hidden = !check.confirm;
    $('confirm').disabled = busy;
    $('install').disabled = busy || !connection || connection.failure || !build || check.blocked || (check.confirm && !$('confirm').checked);
}

function addLink(label, href, parent) {
    const link = document.createElement('a');
    link.textContent = label;
    link.href = href;
    parent.append(link);
}

function showBuild() {
    const build = selectedBuild();
    $('confirm').checked = false;
    $('build-details').replaceChildren();
    $('downloads').replaceChildren();
    $('progress').hidden = true;
    if (build) {
        const target = document.createElement('p');
        target.textContent = build.firmware_version
            ? `Built for ${FIRMWARE_NAMES[build.firmware]} ${build.firmware_version} · API ${build.api_version}`
            : `App API ${build.api_version} · Firmware version not recorded`;
        const contents = document.createElement('p');
        contents.textContent = build.files.length > 1
            ? 'Includes the app + matching in-game help.'
            : 'App only. This release does not include a help file.';
        $('build-details').append(target, contents);
        if (build.data_version) {
            const folder = document.createElement('p');
            folder.textContent = `Help and saved data: apps_data/missile_cmd/${build.data_version}/`;
            $('build-details').append(folder);
        }
        addLink('Release notes ↗', selectedRelease().url, $('downloads'));
        if (build.bundle_url) addLink('Download ZIP', build.bundle_url, $('downloads'));
        else for (const file of build.files) addLink(file.name.endsWith('.fap') ? 'Download FAP' : 'Download help', file.url, $('downloads'));
        status('Ready when you are. Connect your Flipper to continue.');
    }
    refresh();
}

function showRelease() {
    const previous = device?.family || $('firmware').value;
    const builds = selectedRelease()?.builds || [];
    $('firmware').replaceChildren(...builds.map((build) => new Option(FIRMWARE_NAMES[build.firmware], build.firmware)));
    if (builds.some((build) => build.firmware === previous)) $('firmware').value = previous;
    showBuild();
}

$('release').addEventListener('change', showRelease);
$('firmware').addEventListener('change', showBuild);
$('confirm').addEventListener('change', refresh);

$('connect').addEventListener('click', async () => {
    if (busy) return;
    busy = true;
    refresh();
    let candidate;
    try {
        // Keep requestPort directly in the click handler to preserve user activation.
        const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x0483, usbProductId: 0x5740 }] });
        status('Reading your Flipper’s firmware and microSD card…');
        candidate = new FlipperSerial(port, {
            onDisconnect: (error) => {
                device = null;
                status(error.message, 'error');
                if (!busy) {
                    const disconnected = connection;
                    connection = null;
                    $('device').textContent = 'No device connected';
                    void disconnected?.close();
                    refresh();
                }
            }
        });
        device = await candidate.open();
        connection = candidate;
        $('device').textContent = `${FIRMWARE_NAMES[device.family] || 'Firmware'} ${device.version} · microSD ready`;
        showRelease();
        status('Connected. Review the build, then install.');
    } catch (error) {
        await candidate?.close();
        connection = null;
        device = null;
        $('device').textContent = 'No device connected';
        status(error.name === 'NotFoundError' ? 'No device selected. Connect when you’re ready.' : error.message, 'error');
    } finally {
        busy = false;
        refresh();
    }
});

$('disconnect').addEventListener('click', async () => {
    busy = true;
    refresh();
    await connection?.close();
    connection = null;
    device = null;
    $('device').textContent = 'No device connected';
    $('confirm').checked = false;
    busy = false;
    status('Disconnected. You can unplug your Flipper.');
    refresh();
});

$('install').addEventListener('click', async () => {
    if ($('install').disabled || busy) return;
    const build = selectedBuild();
    const release = selectedRelease();
    const active = connection;
    busy = true;
    refresh();
    $('progress').hidden = false;
    $('progress').value = 0;
    status('Downloading and checking release files…');
    try {
        const files = await Promise.all(build.files.map((file) => fetchFile(file)));
        const total = files.reduce((sum, file) => sum + file.size, 0);
        let sent = 0;
        await active.install(files, (bytes, message) => {
            sent += bytes;
            $('progress').value = Math.min(95, sent / total * 95);
            if (message) status(message);
        }, build.data_version ?? null);
        $('progress').value = 100;
        status(`${release.tag} installed and verified${files.length > 1 ? ', including help' : ''}. Open Apps → Games → Missile CMD on your Flipper.`, 'success');
    } catch (error) {
        status(error.message, 'error');
    } finally {
        // Release USB even on error. A timed-out binary transfer cannot be reused.
        await active.close();
        connection = null;
        device = null;
        $('device').textContent = 'Disconnected · USB connection released';
        busy = false;
        refresh();
    }
});

window.addEventListener('beforeunload', (event) => {
    if (busy) { event.preventDefault(); event.returnValue = ''; }
});

if (!supported) {
    $('browser-notice').hidden = false;
    $('browser-notice').textContent = 'USB installation needs desktop Chrome or Edge on an HTTPS page (or localhost). You can still download the files below and copy them with qFlipper.';
}

try {
    const response = await fetch('./catalog.json', { cache: 'no-cache', signal: AbortSignal.timeout(20000) });
    if (!response.ok) throw new Error('The release catalog could not be loaded. Reload the page or use GitHub releases.');
    const catalog = await response.json();
    if (catalog.schema !== 1 || !Array.isArray(catalog.releases)) throw new Error('Unsupported release catalog. Reload the page.');
    releases = catalog.releases.filter((release) => release.builds.length);
    if (!releases.length) throw new Error('No installable releases have been published yet.');
    $('release').replaceChildren(...releases.map((release) => new Option(`${release.tag}${release.prerelease ? ' · prerelease' : ''}`, release.tag)));
    $('release').value = (releases.find((release) => !release.prerelease) || releases[0]).tag;
    $('source').href = `https://github.com/${catalog.repository}`;
    showRelease();
} catch (error) {
    $('release').replaceChildren(new Option('Unavailable'));
    $('catalog-error').textContent = error.message;
    $('catalog-error').hidden = false;
    status('Download releases using the Source & releases link above.');
    refresh();
}
