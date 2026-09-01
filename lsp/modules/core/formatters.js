import { el } from './dom_helpers.js';
import { getLocale, tr, trn } from './i18n.js';

export function time(secs,type){
  const d = new Date(secs * 1000);
  return new Intl.DateTimeFormat(getLocale(), {
    hour: '2-digit', minute: '2-digit',
    second: type === 'short' ? undefined : '2-digit'
  }).format(d);
}

export function date(secs,type) {
  const d = new Date(secs * 1000);
  return new Intl.DateTimeFormat(getLocale(), {
    weekday: type === 'long' ? 'short' : undefined,
    day: '2-digit', month: 'short', year: type === 'short' ? undefined : 'numeric'
  }).format(d);
}

export function dateTime(secs,type) {
  const d = new Date(secs * 1000);
  return new Intl.DateTimeFormat(getLocale(), {
    weekday: type === 'long' ? 'short' : undefined,
    day: '2-digit', month: 'short', year: type === 'short' ? undefined : 'numeric',
    hour: '2-digit', minute: '2-digit', second: type === 'short' ? undefined : '2-digit'
  }).format(d);
}

export function duration(seconds,notimestamp) {
  const multiplications = [1e-3,  1e3,   60,  60,   24, 1e99];
  const units =           ['ms','sec','min','hr','day'];
  const amounts = {};
  const minus = !!(seconds < 0);
  let left = Math.abs(seconds);
  for (const i in units) {
    left = Math.round(left / multiplications[i]);
    const amount = left % multiplications[Number(i)+1];
    amounts[units[i]] = amount;

    left -= amount;
  }

  let unit;
  for (let i = units.length-1; i >= 0; i--) {
    const amount = amounts[units[i]];
    if (amounts[units[i]] > 0) {
      unit = units[i];
      break;
    }
  }
  const s = el('span');
  switch (unit) {
    case 'day':
      if (notimestamp) {
        s.innerHTML = trn('%s day, ', '%s days, ', amounts.day, amounts.day) + trn('%s hour', '%s hours', amounts.hr, amounts.hr);
        break;
      }
      else {
        s.innerHTML = trn('%s day, ', '%s days, ', amounts.day, amounts.day);
      }
    default:
      if (notimestamp) {
        switch (unit) {
          case "hr": {
            s.innerHTML = trn('%s hour, ', '%s hours, ', amounts.hr, amounts.hr) + trn('%s minute', '%s minutes', amounts.min, amounts.min);
            break;
          }
          case "min": {
            s.innerHTML = trn('%s minute, ', '%s minutes, ', amounts.min, amounts.min) + trn('%s second', '%s seconds', amounts.sec, amounts.sec);
            break;
          }
          case "sec": {
            const v = Math.round(amounts.sec*1000 + amounts.ms)/1000;
            s.innerHTML = addUnit(v,'s');
            break;
          }

          case "ms": {
            s.innerHTML = addUnit(amounts.ms,'ms');
            break;
          }
        }
      }
      else {
        s.innerHTML = [
          ('0'+amounts.hr).slice(-2),
          ('0'+amounts.min).slice(-2),
          ('0'+amounts.sec).slice(-2)+(amounts.ms ? '.'+('00'+amounts.ms).slice(-3) : '')
        ].join(':');
      }
      break;
  }
  const out =  (minus ? "- " : "")+s.innerHTML;
  return out;
}

export function number(num,opts) {
  if ((isNaN(Number(num))) || (num == 0)) { return num; }

  opts = Object.assign({
    round: true
  },opts);

  return new Intl.NumberFormat(getLocale(), opts.round ? {
    maximumSignificantDigits: 3
  } : {
    maximumFractionDigits: 20
  }).format(Number(num));
}

export function status(item) {
  const s = el('span');

  if (typeof item.online == 'undefined') {
    s.textContent = tr('Unknown, checking..');
    if (typeof item.error != 'undefined') {
      s.textContent = item.error;
    }
    return s;
  }

  switch (item.online) {
    case -1: s.textContent = tr('Enabling'); break;
    case  0: s.textContent = tr('Unavailable'); s.classList.add('red'); break;
    case  1: s.textContent = tr('Active'); s.classList.add('green'); break;
    case  2: s.textContent = tr('Standby'); s.classList.add('orange'); break;
    default: s.textContent = item.online;
  }
  if ('error' in item) {
    s.textContent = item.error;
  }
  return s;
}

export function capital(string) {
  return string.charAt(0).toUpperCase() + string.substring(1);
}

export function addUnit(num,unit){
  const s = el('span');
  s.innerHTML = num;
  s.appendChild(
    el('span', {class: 'unit'}, unit)
  );
  return s.innerHTML;
}

export function bitbytes(val,opts){
  opts = Object.assign({
    persec: false,
    bytes: false,
    base: 1000,
    info: true
  },opts);

  let suffix = {
    bits: {
      1000: ['bit','kbit','Mbit','Gbit','Tbit','Pbit','Ebit','Zbit'],
      1024: ['bit','Kib','Mib','Gib','Tib','Pib','Eib','Zib']
    },
    bytes: {
      1000: ['byte','kbyte','Mbyte','Gbyte','Tbyte','Pbyte','Ebyte','Zbyte'],
      1024: ['byte','KiB','MiB','GiB','TiB','PiB','EiB','ZiB']
    }
  };
  if (!(opts.base in suffix[opts.bytes ? "bytes" : "bits"])) {
    opts.base = 1000;
  }
  suffix = suffix[[opts.bytes ? "bytes" : "bits"]][opts.base];
  let persec = "";
  if (opts.persec) {
    persec = "/s";
  }

  let newval = val;
  let unit;
  if (newval == 0) {
    unit = suffix[0];
  }
  else {
    const exponent = Math.floor(Math.log(Math.abs(val)) / Math.log(opts.base));
    if (exponent < 0) {
      unit = suffix[0];
    }
    else {
      newval = newval / Math.pow(opts.base,exponent);
      unit = suffix[exponent];
    }
  }
  if ((unit == suffix[0]) && (newval != 1)) {
    unit += "s";
  }

  const span = el('span', {}, number(newval));
  const unitSpan = el('span', {class: 'unit'}, unit+persec);

  if (opts.info && (val != 0)) {
    const infoSpan = el('span', {class: 'info', 'data-icon': 'info'});
    infoSpan.addEventListener('mouseenter', function(e){
      const header = el('h3');
      header.innerHTML = addUnit(number(newval),unit+persec);
      if (newval != val) {
        header.innerHTML += ": "+addUnit(number(Math.round(val),{round:false}),(opts.bytes ? "bytes" : "bits")+persec);
      }

      const li1 = el('li');
      li1.appendChild(bitbytes(val,{
        bytes: opts.bytes,
        persec: opts.persec,
        base: opts.base == 1000 ? 1024 : 1000,
        info: false
      }));

      const li2 = el('li');
      li2.appendChild(bitbytes(opts.bytes ? val*8 : val/8,{
        bytes: !opts.bytes,
        persec: opts.persec,
        base: opts.base,
        info: false
      }));

      const li3 = el('li');
      li3.appendChild(bitbytes(opts.bytes ? val*8 : val/8,{
        bytes: !opts.bytes,
        persec: opts.persec,
        base: opts.base == 1000 ? 1024 : 1000,
        info: false
      }));

      const tooltip = el('div');
      tooltip.appendChild(header);
      tooltip.appendChild(el('p', {}, "These are "+(opts.bytes ? "bytes" : "bits")+(persec == "" ? "" : " per second")+" with a base of "+opts.base+" ("+(opts.base == 1000 ? "decimal" : "binary")+tr("). This equals:")));
      const ul = el('ul');
      ul.appendChild(li1);
      ul.appendChild(li2);
      ul.appendChild(li3);
      tooltip.appendChild(ul);

      UI.tooltip.show(e, tooltip);
    });
    infoSpan.addEventListener('mouseleave', function(){
      UI.tooltip.hide();
    });
    unitSpan.appendChild(infoSpan);
  }

  span.appendChild(unitSpan);
  return span;
}

export function bytes(val,persec){
  return bitbytes(val,{bytes: true, persec: persec, base: 1024});
}

export function bits(val,persec){
  return bitbytes(val,{persec: persec, base: 1000});
}
