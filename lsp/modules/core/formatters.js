import { el } from './dom_helpers.js';

export function time(secs,type){
  const d = new Date(secs * 1000);
  const str = [];
  str.push(('0'+d.getHours()).slice(-2));
  str.push(('0'+d.getMinutes()).slice(-2));
  if (type != 'short') { str.push(('0'+d.getSeconds()).slice(-2)); }
  return str.join(':');
}

export function date(secs,type) {
  const d = new Date(secs * 1000);
  const days = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
  const months = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
  const str = [];
  if (type == 'long') { str.push(days[d.getDay()]); }
  str.push(('0'+d.getDate()).slice(-2));
  str.push(months[d.getMonth()]);
  if (type != 'short') { str.push(d.getFullYear()); }
  return str.join(' ');
}

export function dateTime(secs,type) {
  return date(secs,type)+', '+time(secs,type);
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
        s.innerHTML = addUnit(amounts.day,'days, ') + addUnit(amounts.hr,'hrs');
        break;
      }
      else {
        s.innerHTML = addUnit(amounts.day,'days, ');
      }
    default:
      if (notimestamp) {
        switch (unit) {
          case "hr": {
            s.innerHTML = addUnit(amounts.hr,'hrs, ') + addUnit(amounts.min,'mins');
            break;
          }
          case "min": {
            s.innerHTML = addUnit(amounts.min,'mins, ') + addUnit(amounts.sec,'s');
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

  if (opts.round) {
    const sig = 3;
    const mult = Math.pow(10,sig - Math.floor(Math.log(num)/Math.LN10) - 1);
    num = Math.round(num * mult) / mult;
  }

  if (num >= 1e4) {
    const separator = ' ';
    const parts = num.toString().split('.');
    const regex = /(\d+)(\d{3})/;
    while (regex.test(parts[0])) {
      parts[0] = parts[0].replace(regex,'$1'+separator+'$2');
    }
    num = parts.join('.');
  }

  return num;
}

export function status(item) {
  const s = el('span');

  if (typeof item.online == 'undefined') {
    s.textContent = 'Unknown, checking..';
    if (typeof item.error != 'undefined') {
      s.textContent = item.error;
    }
    return s;
  }

  switch (item.online) {
    case -1: s.textContent = 'Enabling'; break;
    case  0: s.textContent = 'Unavailable'; s.classList.add('red'); break;
    case  1: s.textContent = 'Active'; s.classList.add('green'); break;
    case  2: s.textContent = 'Standby'; s.classList.add('orange'); break;
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
      tooltip.appendChild(el('p', {}, "These are "+(opts.bytes ? "bytes" : "bits")+(persec == "" ? "" : " per second")+" with a base of "+opts.base+" ("+(opts.base == 1000 ? "decimal" : "binary")+"). This equals:"));
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
