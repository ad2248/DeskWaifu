use deskwaifu_runtime::Runtime;
use deskwaifu_test_plugin::TestPlugin;

fn main() -> Result<(), String> {
    let mut runtime = Runtime::new(1024);
    runtime.add(TestPlugin::new())?;
    runtime.start()?;

    let argv: Vec<String> = std::env::args().skip(1).collect();
    let output = if argv.is_empty() {
        runtime.help_text()
    } else {
        runtime.dispatch_command(&argv)?
    };
    println!("{output}");
    runtime.stop()
}
