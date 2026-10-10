import {state} from './state.js';

let nextNameId = 1;

export default {
    DBusCallFlags: {NONE: 0, NO_AUTO_START: 1},
    BusNameOwnerFlags: {NONE: 0},
    DBusError: {SERVICE_UNKNOWN: 2, NAME_HAS_NO_OWNER: 3},
    DBus: {
        session: {
            call(name, path, iface, method, params, _replyType, flags) {
                state.dbusCalls.push({name, path, iface, method, params, flags});
            },
        },
    },
    DBusExportedObject: {
        wrapJSObject(xml, impl) {
            return {
                export(_connection, path) {
                    this.path = path;
                    state.exported.push(path);
                    state.service = impl;
                    state.serviceXml = xml;
                },
                unexport() {
                    state.exported = state.exported.filter(p => p !== this.path);
                    state.service = null;
                },
            };
        },
    },
    bus_own_name_on_connection(_connection, name) {
        const id = nextNameId++;
        state.ownedNames.set(id, name);
        return id;
    },
    bus_unown_name(id) {
        if (!state.ownedNames.delete(id))
            throw new Error(`unown of unknown name id ${id}`);
    },
};
