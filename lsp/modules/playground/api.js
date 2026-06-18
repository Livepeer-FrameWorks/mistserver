import { el } from '../core/dom_helpers.js';

export const connectionStatus = el('div', {id: 'pg-connection'});

let apiBase = location.origin;
let httpHost = null;

/**
 * POST to {apiBase}/api. The apiBase is set by setApiBase() - defaults to
 * location.origin on boot, then gets updated when the user changes Base URL.
 */
export function send(command) {
  return new Promise(function (resolve, reject) {
    connectionStatus.textContent = 'Connecting..';
    connectionStatus.removeAttribute('data-status');
    fetch(apiBase + '/api', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(command),
    }).then(function (r) {
      connectionStatus.setAttribute('data-status', 'connected');
      connectionStatus.textContent = 'Connected';
      if (r.status < 400) {
        r.json().then(resolve).catch(function (e) {
          connectionStatus.setAttribute('data-status', 'failed');
          connectionStatus.textContent = 'Could not parse response';
          reject(e);
        });
      } else {
        reject('HTTP ' + r.status);
      }
    }).catch(function (e) {
      connectionStatus.setAttribute('data-status', 'failed');
      connectionStatus.textContent = 'Failed to connect';
      reject(e);
    });
  });
}

/**
 * Set the management API base URL. Called on init and when Base URL changes.
 */
export function setApiBase(url) {
  apiBase = (url || location.origin).replace(/\/+$/, '');
}

/**
 * Query the management API for protocol config, find the HTTP connector,
 * and derive the HTTP endpoint URL (e.g. http://host:8080).
 * Works with any MistServer - user can point to remote instances.
 */
export function detectHttpHost() {
  return send({ config: true }).then(function (data) {
    const protocols = (data.config && data.config.protocols) || [];
    for (const p of protocols) {
      const name = (p.connector || '').replace('.exe', '').toUpperCase();
      if (name === 'HTTP' || name === 'HTTPS') {
        if (p.pubaddr) {
          const addr = Array.isArray(p.pubaddr) ? p.pubaddr[0] : p.pubaddr;
          if (addr) {
            httpHost = addr.replace(/\/$/, '');
            if (httpHost.indexOf('://') < 0) {
              httpHost = (name === 'HTTPS' ? 'https://' : 'http://') + httpHost;
            }
            return;
          }
        }
        // No pubaddr - derive from the apiBase hostname + connector port
        const port = p.port || (name === 'HTTPS' ? 4433 : 8080);
        let hostname;
        try { hostname = new URL(apiBase).hostname; } catch (e) { hostname = location.hostname; }
        httpHost = (name === 'HTTPS' ? 'https' : 'http') + '://' + hostname + ':' + port;
        return;
      }
    }
    // No HTTP connector found - fall back to apiBase hostname:8080
    let hostname;
    try { hostname = new URL(apiBase).hostname; } catch (e) { hostname = location.hostname; }
    httpHost = location.protocol + '//' + hostname + ':8080';
  }).catch(function () {
    let hostname;
    try { hostname = new URL(apiBase).hostname; } catch (e) { hostname = location.hostname; }
    httpHost = location.protocol + '//' + hostname + ':8080';
  });
}

export function getHttpHost() {
  return httpHost || (location.protocol + '//' + location.hostname + ':8080');
}

export function fetchStreamSources(streamName) {
  const base = getHttpHost();
  const url = base + '/json_' + encodeURIComponent(streamName) + '.js';
  return fetch(url).then(function (r) {
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  });
}
