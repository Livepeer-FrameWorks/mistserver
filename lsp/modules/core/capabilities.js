export function findInput(name) {
  return findInOutput('inputs', name);
}

export function findOutput(name) {
  return findInOutput('connectors', name);
}

export function findInOutput(where, name) {
  if ('capabilities' in mist.data) {
    let output = false;
    const loc = mist.data.capabilities[where];
    if (name in loc) { output = loc[name]; }
    if (name+'.exe' in loc) { output = loc[name+'.exe']; }
    return output;
  }
  else {
    throw 'Request capabilities first';
  }
}
