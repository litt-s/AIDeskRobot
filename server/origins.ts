/*
 * Which browser origins may call the API.
 *
 * Same-origin requests are always fine, and in production that is the only
 * thing accepted besides nothing at all. Development is different: the machine
 * running the dev server is also reached over its LAN address, and that address
 * changes with every network it joins (Wi-Fi, hotspot, dock). Listing them by
 * hand means the console breaks every time the user moves, so instead of a list
 * this accepts any origin whose host is a loopback or private address literal.
 *
 * That is narrow in the way that matters. A page served from a public domain
 * cannot present a private address as its origin, and DNS rebinding cannot
 * either: the Origin header carries the name the page was loaded from, not the
 * address that name resolved to. An attacker would have to already control
 * content served from inside the user's own network.
 *
 * DEV_ALLOWED_ORIGINS stays for the cases this does not cover - a tunnel or a
 * hostname that resolves to a private address, for example - and is still
 * checked as an exact match.
 */

const LOOPBACK_NAMES = ['localhost', '127.0.0.1', '[::1]', '::1'];

function isPrivateHostname(hostname: string) {
  if (LOOPBACK_NAMES.includes(hostname)) return true;
  const octets = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(hostname);
  if (!octets) return false;
  const parts = octets.slice(1).map(Number);
  if (parts.some(value => value > 255)) return false;
  const [first, second] = parts;
  if (first === 10 || first === 127) return true;            // private, loopback
  if (first === 192 && second === 168) return true;          // private
  if (first === 172 && second >= 16 && second <= 31) return true; // private
  if (first === 169 && second === 254) return true;          // link-local
  return false;
}

/** A bare scheme://host[:port]: no path, credentials, query or default port. */
function parseBareOrigin(origin: string) {
  let url: URL;
  try { url = new URL(origin); } catch { return null; }
  if (url.protocol !== 'http:' && url.protocol !== 'https:') return null;
  if (url.username || url.password || url.search || url.hash || url.pathname !== '/') return null;
  if (url.origin !== origin) return null;   // rejects a redundant default port
  return url;
}

export function trustedOrigin(origin: string | undefined, requestURL: string, environment: string, configured = '') {
  if (!origin || origin === 'null') return false;
  if (origin === new URL(requestURL).origin) return true;
  if (environment !== 'development') return false;
  if (configured.split(',').some(value => value.trim() === origin)) return true;
  const url = parseBareOrigin(origin);
  return url !== null && isPrivateHostname(url.hostname);
}
