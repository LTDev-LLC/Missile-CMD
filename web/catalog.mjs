export const FIRMWARE_NAMES = { official: 'Official', unleashed: 'Unleashed', momentum: 'Momentum' };

// SemVer is one directory component; keep the full prerelease/build suffix.
export function helpTarget(version) {
    const number = '(?:0|[1-9][0-9]*)';
    const identifier = '(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)';
    const semver = new RegExp(`^${number}\\.${number}\\.${number}(?:-${identifier}(?:\\.${identifier})*)?(?:\\+[0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*)?$`);
    if (typeof version !== 'string' || version.length > 255 || semver.exec(version)?.[0] !== version) {
        throw new Error('Invalid release version for the installation folder.');
    }
    return `/ext/apps_data/missile_cmd/${version}/help.bin`;
}

export function compatibility(device, build) {
    if (!device || !build) return { blocked: true, message: 'Connect to check your firmware.' };
    if (device.family && device.family !== build.firmware) {
        return { blocked: true, message: `Your device uses ${FIRMWARE_NAMES[device.family]}. Select that firmware to continue.` };
    }
    if (device.api && device.api.split('.')[0] !== build.api_version.split('.')[0]) {
        return { blocked: true, message: `Firmware API ${device.api} does not match this app’s API ${build.api_version}. Choose a compatible release.` };
    }
    if (device.family === build.firmware && build.firmware_version &&
        device.version.replace(/^v/, '') === build.firmware_version.replace(/^v/, '')) {
        return { blocked: false, confirm: false, message: 'Your firmware matches this build’s target.' };
    }
    return {
        blocked: false, confirm: true, message: build.firmware_version
            ? `Built for ${FIRMWARE_NAMES[build.firmware]} ${build.firmware_version}. Your device reports ${device.version}; compatibility is not confirmed.`
            : `App API ${build.api_version}. This older release has no firmware version metadata; check compatibility before installing.`
    };
}

export async function fetchFile(file, { fetcher = fetch, base = globalThis.location?.href } = {}) {
    const url = new URL(file.url, base);
    if (url.origin !== new URL(base).origin) throw new Error('Install files must come from this site.');
    const response = await fetcher(url, { cache: 'no-store', signal: AbortSignal.timeout(30000) });
    if (!response.ok) throw new Error(`Could not download ${file.name}. Reload the page and retry.`);
    const data = new Uint8Array(await response.arrayBuffer());
    if (data.length !== file.size) throw new Error(`Incomplete download: ${file.name}.`);
    const digest = await crypto.subtle.digest('SHA-256', data);
    const hash = Array.from(new Uint8Array(digest), (byte) => byte.toString(16).padStart(2, '0')).join('');
    if (hash !== file.sha256) throw new Error(`Download checksum mismatch: ${file.name}. Reload the page and retry.`);
    return { ...file, data };
}
