// desktop 文本剪贴板请求协议；取消不代表撤销已提交的剪贴板内容。
#pragma once

namespace ClipboardEvents {
constexpr auto WriteTextCapability = "desktop.clipboard.write_text/v1";
constexpr auto WriteText = "clipboard.write_text";
constexpr auto WriteResult = "clipboard.write_result";
constexpr auto CommandCapability = "command.write_clipboard/v1";
constexpr int MaxTextBytes = 1024 * 1024;
constexpr int DeadlineMs = 3000;
// WriteText data: text(QString), requestToken(QString，可选)。
// WriteResult data: ok(bool), error_code(QString), error(QString),
// copy_state(QString: committed/failed/unknown), requestToken(若请求提供)。
// 外部 command.invoke data.json 是对象字符串：command="write_clipboard", text，
// 可选 requestToken；command.result data.json 为相同结果字段的 JSON 对象字符串。
}
