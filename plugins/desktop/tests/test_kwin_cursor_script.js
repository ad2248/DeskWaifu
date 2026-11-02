// 不安装脚本，直接测试应用实际提供的脚本。
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const vm = require('node:vm');
const timers = [];
const calls = [];
function QTimer() {
    this.timeout = { connect: callback => { this.tick = callback; } };
    this.start = () => { this.running = true; };
    this.stop = () => { this.running = false; };
    timers.push(this);
}
const workspace = {
    cursorPos: { x: -12, y: 42.5 },
    screens: [{ name: 'DP-1', geometry: { x: -200, y: 0, width: 1000, height: 800 } }],
};
vm.runInNewContext(execFileSync(process.argv[2], ['--script'], { encoding: 'utf8' }), {
    QTimer, workspace, callDBus: (...args) => calls.push(args),
});
const [sample, watchdog] = timers;
assert.equal(sample.interval, 30);
assert.equal(watchdog.interval, 2000);
assert.ok(sample.running && watchdog.running);
sample.tick();
assert.equal(calls.length, 1);
sample.tick();
assert.equal(calls.length, 1); // 最多保留一个尚未完成的采样。
let call = calls.shift();
assert.deepEqual(call.slice(0, 4), [':1.42', '/org/deskwaifu/Cursor', 'org.deskwaifu.Cursor', 'Sample']);
assert.deepEqual(call.slice(4, -1), ['-12', '42.5', '', '0', '0', '0', '0']);
// 整数和小数 JS 坐标必须使用相同的 D-Bus 签名。
call.at(-1)('DP-1');
sample.tick();
call = calls.shift();
assert.deepEqual(call.slice(4, -1), ['-12', '42.5', 'DP-1', '-200', '0', '1000', '800']);
call.at(-1)('DP-1');
workspace.cursorPos = { x: 1500, y: -50 };
workspace.screens[0].geometry.width = 1500;
sample.tick();
call = calls.shift();
assert.deepEqual(call.slice(4, -1), ['1500', '-50', 'DP-1', '-200', '0', '1500', '800']);
call.at(-1)('missing');
sample.tick();
call = calls.shift();
assert.deepEqual(call.slice(6, -1), ['', '0', '0', '0', '0']);
watchdog.tick();
call = calls.shift();
assert.equal(call[3], 'NameHasOwner');
assert.equal(call[4], ':1.42');
call.at(-1)(true);
assert.ok(sample.running);
watchdog.tick();
calls.shift().at(-1)(false);
assert.ok(!sample.running && !watchdog.running);
assert.deepEqual(calls.shift(), ['org.kde.KWin', '/Scripting', 'org.kde.kwin.Scripting', 'unloadScript', 'deskwaifu-test']);
