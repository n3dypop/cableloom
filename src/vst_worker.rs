use std::process::Command;
use std::time::{Duration, Instant};

pub fn run(args: &[String]) -> Result<(), String> {
    let executable = std::env::current_exe().map_err(|error| error.to_string())?;
    let worker = executable
        .parent()
        .ok_or("Cannot locate the Cableloom executable directory.")?
        .join("cableloom-vst-worker.exe");
    if !worker.is_file() {
        return Err(format!(
            "VST3 worker not found: {}. Build and place cableloom-vst-worker.exe next to cableloom.exe.",
            worker.display()
        ));
    }
    let mut child = Command::new(&worker)
        .args(args)
        .spawn()
        .map_err(|error| format!("Start VST3 worker: {error}"))?;
    let timeout = command_timeout(args);
    let status = if let Some(timeout) = timeout {
        let deadline = Instant::now() + timeout;
        loop {
            if let Some(status) = child
                .try_wait()
                .map_err(|error| format!("Wait for VST3 scanner: {error}"))?
            {
                break status;
            }
            if Instant::now() >= deadline {
                child
                    .kill()
                    .map_err(|error| format!("Stop timed-out VST3 worker: {error}"))?;
                let _ = child.wait();
                return Err(format!(
                    "VST3 {} timed out after {} seconds; the worker was stopped.",
                    args.first().map_or("command", String::as_str),
                    timeout.as_secs()
                ));
            }
            std::thread::sleep(Duration::from_millis(50));
        }
    } else {
        child
            .wait()
            .map_err(|error| format!("Wait for VST3 worker: {error}"))?
    };
    if status.success() {
        Ok(())
    } else {
        Err(format!(
            "VST3 worker failed with exit code {}.",
            status
                .code()
                .map_or_else(|| "terminated".into(), |code| code.to_string())
        ))
    }
}

fn command_timeout(args: &[String]) -> Option<Duration> {
    match args.first().map(String::as_str) {
        Some("scan" | "list-endpoints") => Some(Duration::from_secs(15)),
        Some("benchmark") => Some(Duration::from_secs(300)),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finite_commands_have_timeouts_but_live_does_not() {
        let args = |command: &str| vec![command.to_string()];
        assert_eq!(
            command_timeout(&args("scan")),
            Some(Duration::from_secs(15))
        );
        assert_eq!(
            command_timeout(&args("list-endpoints")),
            Some(Duration::from_secs(15))
        );
        assert_eq!(
            command_timeout(&args("benchmark")),
            Some(Duration::from_secs(300))
        );
        assert_eq!(command_timeout(&args("live")), None);
    }
}
