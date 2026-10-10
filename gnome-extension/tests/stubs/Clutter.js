import {state} from './state.js';

export default {
    KEY_Control_L: 0xffe3,
    KEY_Shift_L: 0xffe1,
    KEY_Insert: 0xff63,
    KEY_v: 0x076,
    KeyState: {RELEASED: 0, PRESSED: 1},
    InputDeviceType: {KEYBOARD_DEVICE: 1},
    get_default_backend: () => ({
        get_default_seat: () => ({
            create_virtual_device: () => ({
                notify_keyval: (_time, key, keyState) => state.keyEvents.push([key, keyState]),
            }),
        }),
    }),
};
