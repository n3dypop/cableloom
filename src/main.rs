mod protocol;
#[cfg(windows)] mod win;
#[cfg(windows)] mod gui;
#[cfg(all(windows,not(target_arch="x86_64")))]
compile_error!("Cableloom currently supports Windows x64 only.");
#[derive(Debug,PartialEq)]
enum Action { Gui, List(bool), Diagnose, Install(String), Add(u32), Remove(u32), Delay(u32,u32), Help }
fn id(value:&str) -> Result<u32,String> {
    let n=value.parse::<u32>().map_err(|_|"Cable ID must be 1..8.")?;
    if !(1..=8).contains(&n) { Err("Cable ID must be 1..8.".into()) } else { Ok(n) }
}
fn parse(args:&[String]) -> Result<Action,String> {
    let a:Vec<&str>=args.iter().map(String::as_str).collect();
    match a.as_slice() {
        [] | ["gui"] => Ok(Action::Gui),
        ["list"] => Ok(Action::List(false)),
        ["list","--json"] => Ok(Action::List(true)),
        ["diagnose"] => Ok(Action::Diagnose),
        ["install",path] => Ok(Action::Install((*path).into())),
        ["add",n] => Ok(Action::Add(id(n)?)),
        ["remove",n] => Ok(Action::Remove(id(n)?)),
        ["delay",n,frames] => {
            let frames=frames.parse::<u32>().map_err(|_|"Delay must be 48..480 frames.")?;
            if !(48..=480).contains(&frames) { return Err("Delay must be 48..480 frames.".into()); }
            Ok(Action::Delay(id(n)?,frames))
        },
        ["help"] | ["--help"] | ["-h"] => Ok(Action::Help),
        _ => Err("Unknown arguments. Use 'cableloom --help'.".into()),
    }
}
const HELP:&str = "Cableloom 0.1.0 - Windows virtual audio cable manager\n\n\
Usage:\n  cableloom [gui]\n  cableloom list [--json]\n  cableloom diagnose\n\
  cableloom install <signed-package/cableloom.inf>\n  cableloom add <1..8>\n\
  cableloom remove <1..8>\n  cableloom delay <1..8> <48..480 frames>\n\n\
Install, add and remove require administrator access. The GUI requests elevation\n\
only for these operations. Delay changes require both streams to be stopped and\n\
last until the device restarts. Audio is 48 kHz stereo PCM16/24/32. Driver installation\n\
requires a properly signed package. No latency or reliability advantage is claimed.";
#[cfg(windows)]
fn execute(action:Action) -> Result<(),String> {
    match action {
        Action::Gui => gui::run()?,
        Action::Help => println!("{HELP}"),
        Action::List(json) => {
            let all=win::devices()?;
            let mut failed=false;
            if json {
                if all.iter().any(|d|d.status.is_err()) {
                    return Err("Some cable interfaces could not be read; run 'list' for details.".into());
                }
                println!("[{}]",all.iter().filter_map(|d|d.status.as_ref().ok()).map(protocol::Status::json).collect::<Vec<_>>().join(","));
            } else if all.is_empty() {
                println!("No active Cableloom interfaces. Run 'diagnose' to inspect installed devices.");
            } else {
                for d in all {
                    match d.status { Ok(s)=>println!("{}",s.line()), Err(e)=>{eprintln!("{}: {e}",d.path);failed=true;} }
                }
            }
            if failed { return Err("One or more cable status queries failed.".into()); }
        },
        Action::Diagnose => println!("{}",win::diagnose()?),
        Action::Install(path) => println!("{}",win::stage(&path)?),
        Action::Add(id) => println!("{}",win::add(id)?),
        Action::Remove(id) => println!("{}",win::remove(id)?),
        Action::Delay(id,frames) => {win::delay(id,frames)?;println!("Cable {id}: delay {frames} frames ({:.2} ms).",frames as f64/48.0);},
    }
    Ok(())
}
fn main() {
    let mut args:Vec<String>=std::env::args().skip(1).collect();
    let elevated=args.first().is_some_and(|s|s=="--elevated");
    if elevated {args.remove(0);}
    let action=match parse(&args) {
        Ok(a)=>a, Err(e)=>{eprintln!("{e}");std::process::exit(2);}
    };
    if elevated && !matches!(action,Action::Install(_)|Action::Add(_)|Action::Remove(_)) {
        eprintln!("Invalid administrator helper action.");std::process::exit(2);
    }
    #[cfg(windows)]
    if elevated {
        let result=match action {
            Action::Install(path)=>win::stage(&path),
            Action::Add(id)=>win::add(id),
            Action::Remove(id)=>win::remove(id),
            _=>unreachable!(),
        };
        match result {
            Ok(text)=>{println!("{text}");gui::message(&text,false);},
            Err(e)=>{eprintln!("{e}");gui::message(&e,true);std::process::exit(1);}
        }
        return;
    }
    #[cfg(windows)]
    let result=execute(action);
    #[cfg(not(windows))]
    let result:Result<(),String>=if action==Action::Help {println!("{HELP}");Ok(())} else {Err("Cableloom requires Windows 10 version 2004 or later (x64).".into())};
    if let Err(e)=result {
        eprintln!("cableloom: {e}");
        #[cfg(windows)]
        if elevated {gui::message(&format!("{e}\n\nNo system restart was initiated."),true);}
        std::process::exit(1);
    }
    #[cfg(windows)]
    if elevated {gui::message("Operation completed. Run 'diagnose' to inspect the device state. A Windows restart may be needed to finish installation or removal.",false);}
}
#[cfg(test)]
mod tests {
    use super::*;
    fn p(a:&[&str])->Result<Action,String>{parse(&a.iter().map(|s|s.to_string()).collect::<Vec<_>>())}
    #[test] fn command_boundaries(){
        assert_eq!(p(&["add","8"]),Ok(Action::Add(8)));
        assert!(p(&["add","0"]).is_err());assert!(p(&["remove","9"]).is_err());
        assert_eq!(p(&["delay","1","48"]),Ok(Action::Delay(1,48)));
        assert!(p(&["delay","1","47"]).is_err());assert!(p(&["delay","1","481"]).is_err());
        assert!(p(&["delay","1","-1"]).is_err());assert!(p(&["list","ignored"]).is_err());
        assert!(p(&["install"]).is_err());
    }
}
