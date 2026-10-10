import {state} from './state.js';

export function activateWindow(win) {
    state.activated.push(win);
}

export const layoutManager = {
    getWorkAreaForMonitor: index => ({x: index * 1920, y: 32, width: 1920, height: 1048}),
};

export const wm = {
    addKeybinding(name, _settings, _flags, _modes, handler) {
        state.keybindings.set(name, handler);
    },
    removeKeybinding(name) {
        if (!state.keybindings.delete(name))
            throw new Error(`removeKeybinding of unknown ${name}`);
    },
};
