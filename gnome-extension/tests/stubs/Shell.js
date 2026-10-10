export default {
    ActionMode: {NORMAL: 1, OVERVIEW: 2},
    WindowTracker: {
        get_default: () => ({get_window_app: win => win.app}),
    },
};
