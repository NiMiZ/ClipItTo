// Shared fake GNOME Shell environment for the extension tests. resetState() builds a fresh
// world (selection, display, windows, settings) and records everything the extension does.

export const state = {};

export class Signals {
    constructor() {
        this._handlers = new Map();
        this._nextId = 1;
    }

    connect(name, callback) {
        const id = this._nextId++;
        this._handlers.set(id, {name, callback});
        return id;
    }

    disconnect(id) {
        if (!this._handlers.delete(id))
            throw new Error(`disconnect of unknown handler ${id}`);
    }

    emit(name, ...args) {
        for (const {name: n, callback} of [...this._handlers.values()]) {
            if (n === name)
                callback(this, ...args);
        }
    }

    handlerCount() {
        return this._handlers.size;
    }
}

// Same numbering as Meta.SelectionType and St.ClipboardType.
export const PRIMARY = 0;
export const CLIPBOARD = 1;

class FakeSelection extends Signals {
    constructor() {
        super();
        this.mimeTypes = {};
    }

    get_mimetypes(type) {
        return this.mimeTypes[type] ?? [];
    }

    setOwner(type, mimeTypes, text) {
        this.mimeTypes[type] = mimeTypes;
        state.content[type] = text;
        this.emit('owner-changed', type, null);
    }
}

class FakeDisplay extends Signals {
    get_selection() {
        return state.selection;
    }

    get_current_monitor() {
        return state.currentMonitor;
    }
}

class FakeSettings extends Signals {
    constructor(values) {
        super();
        this.values = values;
    }

    get_strv(key) {
        return this.values[key];
    }

    get_boolean(key) {
        return this.values[key];
    }

    set(key, value) {
        this.values[key] = value;
        this.emit(`changed::${key}`, key);
    }
}

class FakeWindow extends Signals {
    constructor({wmClass = '', instance = '', app = null, frame = {x: 0, y: 0, width: 560, height: 480}}) {
        super();
        this.wmClass = wmClass;
        this.instance = instance;
        this.app = app;
        this.frame = frame;
    }

    get_wm_class() {
        return this.wmClass;
    }

    get_wm_class_instance() {
        return this.instance;
    }

    get_frame_rect() {
        return this.frame;
    }

    move_frame(_userOp, x, y) {
        state.moved.push({window: this, x, y});
    }
}

export function makeWindow(props) {
    return new FakeWindow(props);
}

export function resetState() {
    state.dbusCalls = [];
    state.exported = [];
    state.ownedNames = new Map();
    state.service = null;
    state.textReads = 0;
    state.contentReads = [];
    state.clipboardSet = [];
    state.activated = [];
    state.moved = [];
    state.keyEvents = [];
    state.keybindings = new Map();
    state.windows = [];
    state.currentMonitor = 1;
    state.content = {};
    state.selection = new FakeSelection();
    state.display = new FakeDisplay();
    state.settings = new FakeSettings({'toggle-shortcut': [], 'capture-primary': false});
    globalThis.global = {
        display: state.display,
        get_window_actors: () => state.windows.map(w => ({meta_window: w})),
    };
}
