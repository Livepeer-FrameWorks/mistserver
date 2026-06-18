import { registerTab } from './tab_registry.js';
import { AppShell } from './appshell.js';

const modeAwareTabs = {};

function register(tabName, handlers) {
  modeAwareTabs[tabName] = true;

  registerTab(tabName, function() {
    const mode = AppShell.getMode();
    const handler = handlers[mode] || handlers.advanced;
    handler.apply(this, arguments);
  });
}

export { register, modeAwareTabs };
