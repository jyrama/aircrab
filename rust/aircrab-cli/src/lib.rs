use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};

use clap::{ArgAction, Parser};

#[derive(Parser, Debug)]
#[command(
    name = "shairport-sync",
    version,
    about = "aircrab: AirPlay 1/2 audio server (Rust clap CLI)"
)]
struct Cli {
    /// Configuration file
    #[arg(short = 'c', long = "configfile", value_name = "FILE")]
    configfile: Option<String>,
    /// Port to listen on
    #[arg(short = 'p', long = "port", value_name = "N")]
    port: Option<i32>,
    /// Service name advertised via mDNS
    #[arg(short = 'a', long = "name", value_name = "NAME")]
    name: Option<String>,
    /// Output backend
    #[arg(short = 'o', long = "output", value_name = "BACKEND")]
    output: Option<String>,
    /// Increase debug verbosity (repeatable)
    #[arg(short = 'v', long = "verbose", action = ArgAction::Count)]
    verbose: u8,
    /// Display the configuration
    #[arg(short = 'X', long = "displayConfig", action = ArgAction::SetTrue)]
    display_config: bool,
    /// Kill the running daemon and exit
    #[arg(short = 'k', long = "kill", action = ArgAction::SetTrue)]
    kill: bool,
}

unsafe extern "C" {
    fn aircrab_set_configfile(s: *const c_char);
    fn aircrab_set_port(port: c_int);
    fn aircrab_set_name(s: *const c_char);
    fn aircrab_set_output(s: *const c_char);
    fn aircrab_add_verbose();
    fn aircrab_set_display_config();
    fn aircrab_set_kill();
}

fn to_c(s: &str) -> CString {
    CString::new(s).unwrap_or_else(|_| CString::new("").expect("empty CString"))
}

#[unsafe(no_mangle)]
pub extern "C" fn aircrab_parse_options(
    argc: c_int,
    argv: *const *const c_char,
    should_exit: *mut c_int,
) -> c_int {
    unsafe { *should_exit = 0 };
    let prog = if argv.is_null() {
        "shairport-sync".to_string()
    } else {
        let p0 = unsafe { *argv };
        if p0.is_null() {
            "shairport-sync".to_string()
        } else {
            unsafe { CStr::from_ptr(p0) }.to_string_lossy().into_owned()
        }
    };
    let mut args: Vec<String> = Vec::new();
    if !argv.is_null() {
        for i in 1..argc.max(0) {
            let p = unsafe { *argv.add(i as usize) };
            if p.is_null() {
                break;
            }
            args.push(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned());
        }
    }
    let mut full = vec![prog];
    full.extend(args);
    match Cli::try_parse_from(full) {
        Ok(cli) => {
            if let Some(f) = &cli.configfile {
                unsafe { aircrab_set_configfile(to_c(f).as_ptr()) };
            }
            if let Some(p) = cli.port {
                unsafe { aircrab_set_port(p) };
            }
            if let Some(n) = &cli.name {
                unsafe { aircrab_set_name(to_c(n).as_ptr()) };
            }
            if let Some(o) = &cli.output {
                unsafe { aircrab_set_output(to_c(o).as_ptr()) };
            }
            for _ in 0..cli.verbose {
                unsafe { aircrab_add_verbose() };
            }
            if cli.display_config {
                unsafe { aircrab_set_display_config() };
            }
            if cli.kill {
                unsafe { aircrab_set_kill() };
            }
            0
        }
        Err(e) => {
            let _ = e.print();
            unsafe { *should_exit = 1 };
            if e.use_stderr() { 1 } else { 0 }
        }
    }
}
