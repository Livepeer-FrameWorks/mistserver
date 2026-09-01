const handlers = {};

export function registerTab(name, handler) {
  handlers[name] = handler;
}

export function getTabHandler(name) {
  return handlers[name] || null;
}

export { handlers as tabHandlers };
