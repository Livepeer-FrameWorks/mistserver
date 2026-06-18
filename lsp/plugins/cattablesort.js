// by Carina van der Meer for DDVtech,
// based off the "Stupid jQuery table plugin" by JoeQuery (http://joequery.github.io/Stupid-Table-Plugin/)

export function cattablesort(table) {
  table.addEventListener('click', (ev) => {
    const th = ev.target.closest('thead th');
    if (!th) return;
    stupidsort(th);
  });
}

function stupidsort(th) {
  const table = th.closest('table');
  const tbody = table.querySelector('tbody');
  const trs = Array.from(tbody.children);
  const datatype = th.getAttribute('data-sort-type');
  if (!datatype) return;
  const sortasc = !th.classList.contains('sorting-asc');

  let col_index = 0;
  let prev = th.previousElementSibling;
  while (prev) {
    const colspan = prev.getAttribute('colspan');
    col_index += (colspan ? Number(colspan) : 1);
    prev = prev.previousElementSibling;
  }

  function getsortval(tr) {
    const cells = tr.querySelectorAll('td, th');
    let i = 0;
    let td;
    for (let c = 0; c < cells.length; c++) {
      if (i === col_index) {
        td = cells[c];
        break;
      }
      const colspan = cells[c].getAttribute('colspan');
      i += (colspan ? Number(colspan) : 1);
    }
    if (!td) return '';

    let val;
    if (td.dataset.sortValue !== undefined) {
      val = td.dataset.sortValue;
    } else {
      val = td.textContent;
    }
    switch (datatype) {
      case 'string':
      case 'string-ins':
        val = String(val).toLowerCase();
        break;
      case 'int':
        val = parseInt(Number(val));
        break;
      case 'float':
        val = Number(val);
        break;
    }
    return val;
  }

  trs.sort((a, b) => {
    const factor = (sortasc ? 1 : -1);
    a = getsortval(a);
    b = getsortval(b);
    if (a > b) return factor;
    if (a < b) return -factor;
    return 0;
  });
  for (let i = 0; i < trs.length; i++) tbody.appendChild(trs[i]);

  const ths = table.querySelectorAll('thead th');
  for (let i = 0; i < ths.length; i++) {
    ths[i].classList.remove('sorting-asc', 'sorting-desc');
  }
  th.classList.add(sortasc ? 'sorting-asc' : 'sorting-desc');
}
