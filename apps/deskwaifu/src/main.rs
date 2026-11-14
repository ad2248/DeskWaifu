use deskwaifu_runtime::Runtime;
use deskwaifu_test_plugin::TestPlugin;
use std::time::Duration;

fn main() -> Result<(), String> {
    let (plugin, report) = TestPlugin::new();
    let mut runtime = Runtime::new(1024);
    runtime.add(plugin)?;
    runtime.start()?;
    runtime.run_for(Duration::from_millis(3200))?;
    runtime.stop()?;
    let counts = report.counts();
    if counts.sent == 0 || counts.sent != counts.received {
        return Err(format!("self-message check failed: {counts:?}"));
    }
    println!("self-message check passed: {counts:?}");
    Ok(())
}
