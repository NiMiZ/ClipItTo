import GLib from 'gi://GLib';

import {state} from './state.js';

const clipboard = {
    get_text(type, callback) {
        state.textReads++;
        callback(clipboard, state.content[type] ?? null);
    },
    set_text(type, text) {
        state.clipboardSet.push({type, text});
    },
    get_content(type, mimeType, callback) {
        state.contentReads.push(mimeType);
        const data = state.content[type];
        callback(clipboard, data instanceof Uint8Array ? new GLib.Bytes(data) : null);
    },
    set_content(type, mimeType, bytes) {
        state.clipboardSet.push({type, mimeType, data: Array.from(bytes.toArray())});
    },
};

export default {
    ClipboardType: {PRIMARY: 0, CLIPBOARD: 1},
    Clipboard: {get_default: () => clipboard},
};
