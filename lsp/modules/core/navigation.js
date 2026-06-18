export function navto(tab, other) {
  const prevhash = location.hash;
  const hash = prevhash.split('@');
  hash[0] = [mist.user.name, mist.user.host].join('&');
  hash[1] = [tab, other].join('&');
  if (typeof window.screenlog != 'undefined') { window.screenlog.navto(hash[1]); }
  location.hash = hash.join('@');
  if (location.hash == prevhash) {
    window.dispatchEvent(new Event('hashchange'));
  }
}
