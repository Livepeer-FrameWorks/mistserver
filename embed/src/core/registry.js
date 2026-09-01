export const mistplayers = {};

let nextPriority = 1;

export function registerWrapper(name, definition) {
  definition.shortname = name;
  if (!('priority' in definition)) {
    definition.priority = nextPriority++;
  }
  mistplayers[name] = definition;
}
