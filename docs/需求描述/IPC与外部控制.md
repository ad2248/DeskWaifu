# IPC 与外部控制

宿主监听 `/tmp/deskwaifu.socket`，权限为 `0600`。`deskwaifucli` 和控制面板使用同一宿主协议。CLI 的 `--help` 可离线使用，插件命令由正在运行的宿主分派。固定路径同一时间只能由一个实例绑定。启动不会覆盖已有路径，正常退出会清理自己绑定的 socket。 [宿主](<../../crates/os/src/host.rs>) · [独立 CLI](<../../apps/deskwaifucli/src/main.rs>)

以 LF 分隔 UTF-8 JSON，每行一个完整信封。请求只支持 `source: "cli"`，`payload` 必须为字符串数组。第一个元素是已注册的命令名，余下参数原样传入插件。额外信封字段、错误类型和未知来源被拒绝。

```json
{"source":"cli","payload":["test","你好 world","","001"]}
```

成功响应为 `{"status":"ok","output":"…"}`，失败响应为 `{"status":"error","error":"…"}`。CLI 成功打印输出并退出 0，出错退出非 0。插件可以用输出字符串携带进一步编码的结果。 [字节边界与信封实现](<../../crates/os/src/lib.rs>)

- 支持长连接、拆包和同一连接内的请求顺序。未以 LF 结束的帧不会执行。
- 单条请求和响应上限为 64 KiB，不含末尾 LF。超长请求报错并关闭该连接，超长响应返回错误。
- 并发连接上限为 32，连接空闲超时 10 秒。插件回调在宿主线程执行，传输超时不提供命令的执行期限或取消语义。
- 请求发出后若连接中断，调用方应先查询有效状态。对终端变换编辑等连接独占操作，应由同一连接完成开始、更新和应用。 [异步服务](<../../crates/os/src/async_server.rs>) · [terminal 编辑协议](<../../plugins/terminal/README.md>)
