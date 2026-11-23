const Gio      = imports.gi.Gio;
const GLib     = imports.gi.GLib;
const ByteArray = imports.byteArray;
const Mainloop = imports.mainloop;
const Settings = imports.ui.settings;

const TILE_KEYS = [
    'push-tile-left',
    'push-tile-right',
    'push-tile-up',
    'push-tile-down',
];

const WIN11_DEFAULT_ARROWS = [
    '<Super>Left',
    '<Super>Right',
    '<Super>Up',
    '<Super>Down',
];

const SHORTCUT_KEYS = [
    'snap_layouts',
    'snap_left',
    'snap_right',
    'snap_up',
    'snap_down',
    'snap_top_half',
    'snap_bottom_half',
];

function canonicalAccel(accel) {
    if (!accel) return '';
    let s = String(accel).trim();
    if (!s) return '';
    let mods = [];
    let pos = 0;
    while (pos < s.length && s[pos] === '<') {
        let end = s.indexOf('>', pos);
        if (end < 0) break;
        let tok = s.slice(pos + 1, end).toLowerCase();
        if ((tok === 'ctrl' || tok === 'control' || tok === 'primary') && mods.indexOf('<Control>') < 0) {
            mods.push('<Control>');
        } else if (tok === 'shift' && mods.indexOf('<Shift>') < 0) {
            mods.push('<Shift>');
        } else if ((tok === 'alt' || tok === 'mod1') && mods.indexOf('<Alt>') < 0) {
            mods.push('<Alt>');
        } else if ((tok === 'super' || tok === 'mod4') && mods.indexOf('<Super>') < 0) {
            mods.push('<Super>');
        }
        pos = end + 1;
    }
    let key = s.slice(pos).trim();
    if (!key) return '';
    let order = { '<Control>': 0, '<Shift>': 1, '<Alt>': 2, '<Super>': 3 };
    mods.sort((a, b) => order[a] - order[b]);
    if (key.length === 1) {
        key = key.toLowerCase();
    } else {
        const canonMap = {
            left: 'Left', right: 'Right', up: 'Up', down: 'Down',
            return: 'Return', escape: 'Escape', space: 'space', tab: 'Tab',
        };
        if (canonMap[key.toLowerCase()]) {
            key = canonMap[key.toLowerCase()];
        }
    }
    return mods.join('') + key;
}

function updateIniPreservingComments(path, updates) {
    let lines = [];
    if (GLib.file_test(path, GLib.FileTest.EXISTS)) {
        try {
            let [ok, raw] = GLib.file_get_contents(path);
            if (ok) {
                lines = ByteArray.toString(raw).split('\n');
                if (lines.length > 0 && lines[lines.length - 1] === '') {
                    lines.pop();
                }
            }
        } catch (e) {
            global.logError('win11-snap: failed reading INI: ' + e);
        }
    }

    let out = [];
    let curSec = 'general';
    let seenSections = {};
    let written = {};
    for (let sec in updates) {
        written[sec] = {};
    }

    function flushMissing(sec) {
        if (!updates[sec]) return;
        for (let k in updates[sec]) {
            if (!written[sec][k]) {
                out.push(k + ' = ' + updates[sec][k]);
                written[sec][k] = true;
            }
        }
    }

    for (let i = 0; i < lines.length; i++) {
        let rawLine = lines[i];
        let s = rawLine.trim();
        if (s.startsWith('[') && s.indexOf(']') > 0) {
            if (updates[curSec]) {
                flushMissing(curSec);
            }
            curSec = s.slice(1, s.indexOf(']')).trim().toLowerCase();
            seenSections[curSec] = true;
            out.push(rawLine);
            continue;
        }
        if (s && !s.startsWith('#') && !s.startsWith(';') && s.indexOf('=') > 0 && updates[curSec]) {
            let eq = s.indexOf('=');
            let k = s.slice(0, eq).trim().toLowerCase();
            if (Object.prototype.hasOwnProperty.call(updates[curSec], k)) {
                out.push(k + ' = ' + updates[curSec][k]);
                written[curSec][k] = true;
                continue;
            }
        }
        out.push(rawLine);
    }

    if (updates[curSec]) {
        flushMissing(curSec);
    }

    for (let sec in updates) {
        if (!seenSections[sec]) {
            out.push('');
            out.push('[' + sec + ']');
            for (let k in updates[sec]) {
                out.push(k + ' = ' + updates[sec][k]);
            }
        }
    }

    GLib.file_set_contents(path, out.join('\n') + '\n');
}

class Win11SnapExtension {
    constructor(meta) {
        this._uuid = meta.uuid;
        this._extPath = meta.path;
        this._pid = 0;
        this._childWatchId = 0;
        this._syncTimerId = 0;
        this._destroyed = false;

        this._configDir = GLib.build_filenamev([GLib.get_user_config_dir(), 'win11-snap']);
        this._configPath = GLib.build_filenamev([this._configDir, 'config.ini']);
        this._backupPath = GLib.build_filenamev([this._configDir, 'cinnamon_backup.ini']);
        this._pidFile = GLib.build_filenamev([GLib.get_user_runtime_dir(), 'win11-snapd.pid']);

        this._settings = new Settings.ExtensionSettings(this, this._uuid);
        this._bindSettings();

        this._ensureConfigAndBackup();
        this._syncCinnamonHandoff(false);
        this._syncConfigIni();
        this._startDaemon();
    }

    _bindSettings() {
        const keys = [
            'language',
            'drag_snap',
            'top_snap_bar',
            'enable_snap_assist',
            'enable_animations',
            'csd_decor',
            'enable_shortcuts',
            'hover_delay_ms',
            'gap_px',
            ...SHORTCUT_KEYS,
        ];
        for (let k of keys) {
            this._settings.bind(k, k, () => this._scheduleSync());
        }
        this._ifaceSettings = new Gio.Settings({ schema_id: 'org.cinnamon.desktop.interface' });
        this._wmPrefSettings = new Gio.Settings({ schema_id: 'org.cinnamon.desktop.wm.preferences' });
        this._ifaceChangedIds = [
            this._ifaceSettings.connect('changed::gtk-theme', () => this._scheduleSync()),
            this._ifaceSettings.connect('changed::scaling-factor', () => this._scheduleSync()),
        ];
        this._wmChangedId = this._wmPrefSettings.connect('changed::button-layout', () => this._scheduleSync());
    }

    _scheduleSync() {
        if (this._destroyed) return;
        if (this._syncTimerId > 0) {
            Mainloop.source_remove(this._syncTimerId);
            this._syncTimerId = 0;
        }
        this._syncTimerId = Mainloop.timeout_add(90, () => {
            this._syncTimerId = 0;
            if (!this._destroyed) {
                this._syncCinnamonHandoff(false);
                this._syncConfigIni();
                this._reloadDaemon();
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    _hasKey(kf, group, key) {
        try {
            if (!kf.has_group(group)) return false;
            kf.get_value(group, key);
            return true;
        } catch (_) {
            return false;
        }
    }

    _readBackup() {
        let kf = new GLib.KeyFile();
        if (GLib.file_test(this._backupPath, GLib.FileTest.EXISTS)) {
            try {
                kf.load_from_file(this._backupPath, GLib.KeyFileFlags.NONE);
            } catch (e) {
                global.logError('win11-snap: backup load error: ' + e);
            }
        }
        return kf;
    }

    _ensureConfigAndBackup() {
        GLib.mkdir_with_parents(this._configDir, 0o755);

        if (!GLib.file_test(this._configPath, GLib.FileTest.EXISTS)) {
            let defCfg = GLib.build_filenamev([this._extPath, 'config', 'config.ini']);
            if (GLib.file_test(defCfg, GLib.FileTest.EXISTS)) {
                try {
                    let [ok, raw] = GLib.file_get_contents(defCfg);
                    if (ok) {
                        GLib.file_set_contents(this._configPath, raw);
                    }
                } catch (e) {
                    global.logError('win11-snap: failed copying default config.ini: ' + e);
                }
            }
        }

        try {
            let iface = new Gio.Settings({ schema_id: 'org.cinnamon.desktop.interface' });
            let muffin = new Gio.Settings({ schema_id: 'org.cinnamon.muffin' });
            let wm = new Gio.Settings({ schema_id: 'org.cinnamon.desktop.keybindings.wm' });

            let kf = this._readBackup();
            let changed = false;

            if (!this._hasKey(kf, 'backup', 'gtk_theme')) {
                let curTheme = iface.get_string('gtk-theme');
                if (curTheme === 'Mint-Y-Dark-Aqua-Win11') {
                    curTheme = 'Mint-Y-Dark-Aqua';
                }
                kf.set_string('backup', 'gtk_theme', curTheme);
                changed = true;
            }
            if (!this._hasKey(kf, 'backup', 'edge_tiling')) {
                kf.set_string('backup', 'edge_tiling', muffin.get_boolean('edge-tiling') ? 'true' : 'false');
                changed = true;
            }
            for (let k of TILE_KEYS) {
                if (!this._hasKey(kf, 'backup', k)) {
                    kf.set_string('backup', k, wm.get_strv(k).join(','));
                    changed = true;
                }
            }
            if (changed) {
                kf.save_to_file(this._backupPath);
            }

            let themeDir = GLib.build_filenamev([GLib.get_home_dir(), '.themes', 'Mint-Y-Dark-Aqua-Win11']);
            if (GLib.file_test(themeDir, GLib.FileTest.IS_DIR) &&
                iface.get_string('gtk-theme') !== 'Mint-Y-Dark-Aqua-Win11') {
                iface.set_string('gtk-theme', 'Mint-Y-Dark-Aqua-Win11');
                Gio.Settings.sync();
            }
        } catch (e) {
            global.logError('win11-snap: backup/theme setup error: ' + e);
        }
    }

    _syncCinnamonHandoff(restoring) {
        try {
            let iface = new Gio.Settings({ schema_id: 'org.cinnamon.desktop.interface' });
            let muffin = new Gio.Settings({ schema_id: 'org.cinnamon.muffin' });
            let wm = new Gio.Settings({ schema_id: 'org.cinnamon.desktop.keybindings.wm' });
            let kf = this._readBackup();

            let origEdge = this._hasKey(kf, 'backup', 'edge_tiling')
                ? (kf.get_string('backup', 'edge_tiling').toLowerCase() === 'true')
                : true;

            const restoreKeyFromBackup = (k) => {
                if (!this._hasKey(kf, 'backup', k)) return;
                let raw = kf.get_string('backup', k);
                let vals = raw.split(',').map(s => s.trim()).filter(s => s.length > 0);
                if (wm.get_strv(k).length === 0 && vals.length > 0) {
                    wm.set_strv(k, vals);
                }
            };

            if (restoring) {
                muffin.set_boolean('edge-tiling', origEdge);
                for (let k of TILE_KEYS) {
                    restoreKeyFromBackup(k);
                }
                if (this._hasKey(kf, 'backup', 'gtk_theme')) {
                    let origTheme = kf.get_string('backup', 'gtk_theme').trim();
                    if (origTheme && iface.get_string('gtk-theme') === 'Mint-Y-Dark-Aqua-Win11') {
                        iface.set_string('gtk-theme', origTheme);
                    }
                }
                Gio.Settings.sync();
                return;
            }

            let dragOn = Boolean(this.drag_snap);
            let scOn = Boolean(this.enable_shortcuts);
            muffin.set_boolean('edge-tiling', dragOn ? false : origEdge);

            let claimed = {};
            for (let sk of SHORTCUT_KEYS) {
                let c = canonicalAccel(this[sk]);
                if (c) claimed[c] = true;
            }

            if (scOn) {
                for (let i = 0; i < TILE_KEYS.length; i++) {
                    let k = TILE_KEYS[i];
                    let defC = canonicalAccel(WIN11_DEFAULT_ARROWS[i]);
                    let cur = wm.get_strv(k).map(canonicalAccel);
                    if (claimed[defC]) {
                        if (cur.indexOf(defC) >= 0) {
                            wm.set_strv(k, []);
                        }
                    } else if (cur.length === 0) {
                        restoreKeyFromBackup(k);
                    }
                }
            } else {
                for (let k of TILE_KEYS) {
                    restoreKeyFromBackup(k);
                }
            }

            Gio.Settings.sync();
        } catch (e) {
            global.logError('win11-snap: handoff sync error: ' + e);
        }
    }

    _syncConfigIni() {
        try {
            let snapLayouts = String(this.snap_layouts || '<Super>z');
            let scOn = Boolean(this.enable_shortcuts);
            let shortcutsSection = {};
            for (let sk of SHORTCUT_KEYS) {
                shortcutsSection[sk] = String(this[sk] || '');
            }
            let updates = {
                general: {
                    language: String(this.language || 'en'),
                    drag_snap: this.drag_snap ? 'true' : 'false',
                    top_snap_bar: this.top_snap_bar ? 'true' : 'false',
                    enable_snap_assist: this.enable_snap_assist ? 'true' : 'false',
                    enable_animations: this.enable_animations ? 'true' : 'false',
                    decorate_custom_csd: this.csd_decor ? 'true' : 'false',
                    enable_shortcuts: scOn ? 'true' : 'false',
                    hotkey_super_z: (scOn && canonicalAccel(snapLayouts).length > 0) ? 'true' : 'false',
                    hover_delay_ms: String(parseInt(this.hover_delay_ms, 10) || 450),
                    gap_px: String(parseInt(this.gap_px, 10) || 0),
                },
                shortcuts: shortcutsSection,
            };
            updateIniPreservingComments(this._configPath, updates);
        } catch (e) {
            global.logError('win11-snap: config.ini sync error: ' + e);
        }
    }

    _killPidIfDaemon(pid) {
        if (!pid || pid <= 0) return;
        try {
            let commPath = '/proc/' + pid + '/comm';
            if (GLib.file_test(commPath, GLib.FileTest.EXISTS)) {
                let [ok, raw] = GLib.file_get_contents(commPath);
                if (ok && ByteArray.toString(raw).trim() === 'win11-snapd') {
                    GLib.spawn_async(null, ['/bin/kill', '-TERM', String(pid)], null, GLib.SpawnFlags.DEFAULT, null);
                }
            }
        } catch (e) {
            global.logError('win11-snap: stop existing daemon error: ' + e);
        }
    }

    _stopExistingDaemon() {
        // Leave _childWatchId intact so GLib reaps the deadPid cleanly after SIGTERM.
        this._childWatchId = 0;
        if (this._pid > 0) {
            this._killPidIfDaemon(this._pid);
            this._pid = 0;
        }
        if (GLib.file_test(this._pidFile, GLib.FileTest.EXISTS)) {
            try {
                let [ok, raw] = GLib.file_get_contents(this._pidFile);
                if (ok) {
                    let oldPid = parseInt(ByteArray.toString(raw).trim(), 10);
                    if (oldPid > 0) {
                        this._killPidIfDaemon(oldPid);
                    }
                }
                GLib.unlink(this._pidFile);
            } catch (e) {
                global.logError('win11-snap: pidfile cleanup error: ' + e);
            }
        }
    }

    _startDaemon() {
        this._stopExistingDaemon();
        let binPath = GLib.build_filenamev([this._extPath, 'win11-snapd']);
        if (!GLib.file_test(binPath, GLib.FileTest.IS_EXECUTABLE)) {
            binPath = GLib.find_program_in_path('win11-snapd');
        }
        if (!binPath) {
            global.logError('win11-snap: win11-snapd binary not found');
            return;
        }
        try {
            let [ok, pid] = GLib.spawn_async(
                this._extPath,
                [binPath, '--config', this._configPath],
                null,
                GLib.SpawnFlags.DO_NOT_REAP_CHILD,
                null
            );
            if (!ok || !pid) {
                global.logError('win11-snap: failed spawning win11-snapd');
                return;
            }
            this._pid = pid;
            GLib.file_set_contents(this._pidFile, String(pid) + '\n');
            this._childWatchId = GLib.child_watch_add(GLib.PRIORITY_DEFAULT, pid, (deadPid, _status) => {
                GLib.spawn_close_pid(deadPid);
                if (this._pid === deadPid) {
                    this._pid = 0;
                    this._childWatchId = 0;
                    if (GLib.file_test(this._pidFile, GLib.FileTest.EXISTS)) {
                        try { GLib.unlink(this._pidFile); } catch (_) {}
                    }
                }
            });
        } catch (e) {
            global.logError('win11-snap: spawn error: ' + e);
        }
    }

    _reloadDaemon() {
        if (this._pid > 0) {
            try {
                GLib.spawn_async(null, ['/bin/kill', '-USR1', String(this._pid)], null, GLib.SpawnFlags.DEFAULT, null);
            } catch (e) {
                global.logError('win11-snap: SIGUSR1 error: ' + e);
            }
        }
    }

    destroy() {
        this._destroyed = true;
        if (this._syncTimerId > 0) {
            Mainloop.source_remove(this._syncTimerId);
            this._syncTimerId = 0;
        }
        if (this._ifaceSettings && this._ifaceChangedIds) {
            for (let id of this._ifaceChangedIds) {
                try { this._ifaceSettings.disconnect(id); } catch (_) {}
            }
            this._ifaceChangedIds = [];
        }
        if (this._wmPrefSettings && this._wmChangedId) {
            try { this._wmPrefSettings.disconnect(this._wmChangedId); } catch (_) {}
            this._wmChangedId = 0;
        }
        this._stopExistingDaemon();
        this._syncCinnamonHandoff(true);
        if (this._settings) {
            this._settings.finalize();
            this._settings = null;
        }
    }
}

let _extensionMeta = null;
let _instance = null;

function init(extensionMeta) {
    _extensionMeta = extensionMeta;
}

function enable() {
    if (_instance) return;
    _instance = new Win11SnapExtension(_extensionMeta);
}

function disable() {
    if (_instance) {
        _instance.destroy();
        _instance = null;
    }
}
