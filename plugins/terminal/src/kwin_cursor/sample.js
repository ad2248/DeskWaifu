// Placeholders are replaced by the provider with D-Bus unique names and a
// per-connection script name. None of those values come from configuration.
var destination = '@DESTINATION@';
var owner = '@OWNER@';
var scriptName = '@SCRIPT_NAME@';
var outputName = '';
var generation = '';
var pending = false;
var sampleTimer = new QTimer();
sampleTimer.interval = 30;
sampleTimer.timeout.connect(function () {
    if (pending) return;
    var rect = {x: 0, y: 0, width: 0, height: 0};
    var name = '';
    var screens = workspace.screens;
    for (var i = 0; i < screens.length; ++i) {
        if (outputName && screens[i].name === outputName) {
            rect = screens[i].geometry;
            name = outputName;
            break;
        }
    }
    var pos = workspace.cursorPos;
    var active = workspace.activeWindow;
    // KWin's callDBus permits up to eight arguments before the callback.
    pending = true;
    callDBus(destination, '/org/deskwaifu/Cursor', 'org.deskwaifu.Cursor', 'Sample',
        String(pos.x), String(pos.y), name, String(rect.x), String(rect.y),
        String(rect.width), String(rect.height),
        (active && active.fullScreen ? 'true' : 'false') + '|' + generation + '|' + String(Date.now()),
        function (target) {
            var separator = target.indexOf('\n');
            generation = target.slice(0, separator);
            outputName = target.slice(separator + 1);
            pending = false;
        });
});
sampleTimer.start();

// Backup for a running script if the independent guard itself disappears.
var watchdog = new QTimer();
watchdog.interval = 2000;
watchdog.timeout.connect(function () {
    callDBus('org.freedesktop.DBus', '/org/freedesktop/DBus',
        'org.freedesktop.DBus', 'NameHasOwner', destination, function (alive) {
            if (alive) return;
            sampleTimer.stop();
            watchdog.stop();
            callDBus(owner, '/Scripting', 'org.kde.kwin.Scripting', 'unloadScript', scriptName);
        });
});
watchdog.start();
