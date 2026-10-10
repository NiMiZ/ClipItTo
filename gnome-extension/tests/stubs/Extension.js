import {state} from './state.js';

export class Extension {
    constructor(metadata) {
        this.metadata = metadata;
    }

    getSettings() {
        return state.settings;
    }
}
