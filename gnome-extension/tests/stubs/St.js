import {state} from './state.js';

const clipboard = {
    get_text(type, callback) {
        state.textReads++;
        callback(clipboard, state.content[type] ?? null);
    },
    set_text(type, text) {
        state.clipboardSet.push({type, text});
    },
};

export default {
    ClipboardType: {PRIMARY: 0, CLIPBOARD: 1},
    Clipboard: {get_default: () => clipboard},
};
