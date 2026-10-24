#![allow(non_snake_case)]
use crate::protocol::*;
use std::{
    ffi::c_void,
    mem::{size_of, zeroed},
    ptr::{null, null_mut},
};
pub type Handle = *mut c_void;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Guid {
    pub a: u32,
    pub b: u16,
    pub c: u16,
    pub d: [u8; 8],
}
const INTERFACE: Guid = Guid {
    a: 0x1678cb4e,
    b: 0x9a71,
    c: 0x4c0f,
    d: [0xba, 0xe5, 0x12, 0xef, 0, 0xcb, 0x71, 0x28],
};
const MEDIA: Guid = Guid {
    a: 0x4d36e96c,
    b: 0xe325,
    c: 0x11ce,
    d: [0xbf, 0xc1, 8, 0, 0x2b, 0xe1, 3, 0x18],
};
const INVALID: Handle = -1isize as Handle;
#[repr(C)]
struct DevInfo {
    size: u32,
    class: Guid,
    instance: u32,
    reserved: usize,
}
impl DevInfo {
    fn new() -> Self {
        unsafe {
            let mut v: Self = zeroed();
            v.size = size_of::<Self>() as u32;
            v
        }
    }
}
#[repr(C)]
struct InterfaceInfo {
    size: u32,
    class: Guid,
    flags: u32,
    reserved: usize,
}
#[link(name = "setupapi")]
extern "system" {
    fn SetupDiGetClassDevsW(
        guid: *const Guid,
        enumerator: *const u16,
        parent: Handle,
        flags: u32,
    ) -> Handle;
    fn SetupDiDestroyDeviceInfoList(set: Handle) -> i32;
    fn SetupDiEnumDeviceInterfaces(
        set: Handle,
        device: *const DevInfo,
        guid: *const Guid,
        index: u32,
        info: *mut InterfaceInfo,
    ) -> i32;
    fn SetupDiGetDeviceInterfaceDetailW(
        set: Handle,
        info: *const InterfaceInfo,
        detail: *mut c_void,
        bytes: u32,
        needed: *mut u32,
        device: *mut DevInfo,
    ) -> i32;
    fn SetupDiCreateDeviceInfoList(guid: *const Guid, parent: Handle) -> Handle;
    fn SetupDiCreateDeviceInfoW(
        set: Handle,
        name: *const u16,
        guid: *const Guid,
        description: *const u16,
        parent: Handle,
        flags: u32,
        info: *mut DevInfo,
    ) -> i32;
    fn SetupDiSetDeviceRegistryPropertyW(
        set: Handle,
        info: *const DevInfo,
        property: u32,
        buffer: *const u8,
        size: u32,
    ) -> i32;
    fn SetupDiGetDeviceRegistryPropertyW(
        set: Handle,
        info: *const DevInfo,
        property: u32,
        kind: *mut u32,
        buffer: *mut u8,
        size: u32,
        needed: *mut u32,
    ) -> i32;
    fn SetupDiCallClassInstaller(function: u32, set: Handle, info: *const DevInfo) -> i32;
    fn SetupDiOpenDeviceInfoW(
        set: Handle,
        id: *const u16,
        parent: Handle,
        flags: u32,
        info: *mut DevInfo,
    ) -> i32;
    fn SetupDiEnumDeviceInfo(set: Handle, index: u32, info: *mut DevInfo) -> i32;
    fn SetupDiGetDeviceInstanceIdW(
        set: Handle,
        info: *const DevInfo,
        id: *mut u16,
        count: u32,
        needed: *mut u32,
    ) -> i32;
    fn SetupCopyOEMInfW(
        source: *const u16,
        media: *const u16,
        source_type: u32,
        style: u32,
        destination: *mut u16,
        count: u32,
        needed: *mut u32,
        component: *mut *mut u16,
    ) -> i32;
}
#[link(name = "newdev")]
extern "system" {
    fn DiInstallDevice(
        parent: Handle,
        set: Handle,
        info: *const DevInfo,
        driver: *const c_void,
        flags: u32,
        reboot: *mut i32,
    ) -> i32;
    fn DiUninstallDevice(
        parent: Handle,
        set: Handle,
        info: *const DevInfo,
        flags: u32,
        reboot: *mut i32,
    ) -> i32;
}
#[link(name = "cfgmgr32")]
extern "system" {
    fn CM_Get_DevNode_Status(status: *mut u32, problem: *mut u32, instance: u32, flags: u32)
        -> u32;
}
#[link(name = "kernel32")]
extern "system" {
    fn CreateFileW(
        path: *const u16,
        access: u32,
        share: u32,
        security: *const c_void,
        creation: u32,
        flags: u32,
        template: Handle,
    ) -> Handle;
    fn CloseHandle(handle: Handle) -> i32;
    fn DeviceIoControl(
        handle: Handle,
        code: u32,
        input: *const c_void,
        input_bytes: u32,
        output: *mut c_void,
        output_bytes: u32,
        returned: *mut u32,
        overlapped: *mut c_void,
    ) -> i32;
    fn WaitForSingleObject(handle: Handle, ms: u32) -> u32;
    fn GetExitCodeProcess(handle: Handle, code: *mut u32) -> i32;
}
pub fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(Some(0)).collect()
}
fn error(context: &str) -> String {
    format!("{context}: {}", std::io::Error::last_os_error())
}
fn check(value: i32, context: &str) -> Result<(), String> {
    if value != 0 {
        Ok(())
    } else {
        Err(error(context))
    }
}
struct DevSet(Handle);
impl DevSet {
    fn new(handle: Handle) -> Result<Self, String> {
        if handle == INVALID {
            Err(error("Open device set"))
        } else {
            Ok(Self(handle))
        }
    }
}
impl Drop for DevSet {
    fn drop(&mut self) {
        unsafe {
            SetupDiDestroyDeviceInfoList(self.0);
        }
    }
}
struct File(Handle);
impl Drop for File {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}
fn open(path: &str, write: bool) -> Result<File, String> {
    let path = wide(path);
    let handle = unsafe {
        CreateFileW(
            path.as_ptr(),
            0x80000000 | if write { 0x40000000 } else { 0 },
            3,
            null(),
            3,
            0,
            null_mut(),
        )
    };
    if handle == INVALID {
        Err(error("Open cable control interface"))
    } else {
        Ok(File(handle))
    }
}
pub struct Device {
    pub path: String,
    pub status: Result<Status, String>,
}
fn read_status(path: &str) -> Result<Status, String> {
    let file = open(path, false)?;
    let request = Request {
        version: VERSION,
        size: 8,
    };
    let mut status = Status::default();
    let mut returned = 0;
    unsafe {
        check(
            DeviceIoControl(
                file.0,
                IOCTL_STATUS,
                &request as *const _ as _,
                8,
                &mut status as *mut _ as _,
                80,
                &mut returned,
                null_mut(),
            ),
            "Read cable status",
        )?;
    }
    if returned != 80 {
        return Err("Driver returned an incomplete status record.".into());
    }
    status.validate()?;
    Ok(status)
}
pub fn devices() -> Result<Vec<Device>, String> {
    unsafe {
        let set = DevSet::new(SetupDiGetClassDevsW(&INTERFACE, null(), null_mut(), 0x12))?;
        let mut devices = Vec::new();
        for index in 0.. {
            let mut info: InterfaceInfo = zeroed();
            info.size = size_of::<InterfaceInfo>() as u32;
            if SetupDiEnumDeviceInterfaces(set.0, null(), &INTERFACE, index, &mut info) == 0 {
                if std::io::Error::last_os_error().raw_os_error() == Some(259) {
                    break;
                }
                return Err(error("Enumerate cable interfaces"));
            }
            let mut needed = 0;
            SetupDiGetDeviceInterfaceDetailW(set.0, &info, null_mut(), 0, &mut needed, null_mut());
            if !(8..=65536).contains(&needed) {
                return Err("Invalid device interface path size.".into());
            }
            let mut detail = vec![0u64; (needed as usize).div_ceil(8)];
            *(detail.as_mut_ptr() as *mut u32) = 8;
            check(
                SetupDiGetDeviceInterfaceDetailW(
                    set.0,
                    &info,
                    detail.as_mut_ptr() as _,
                    needed,
                    &mut needed,
                    null_mut(),
                ),
                "Read interface path",
            )?;
            let path = std::slice::from_raw_parts(
                (detail.as_ptr() as *const u8).add(4) as *const u16,
                (needed as usize - 4) / 2,
            );
            let end = path
                .iter()
                .position(|&v| v == 0)
                .ok_or("Interface path is not terminated.")?;
            let path =
                String::from_utf16(&path[..end]).map_err(|_| "Invalid UTF-16 interface path.")?;
            let status = read_status(&path);
            devices.push(Device { path, status });
        }
        devices.sort_by_key(|d| d.status.as_ref().map(|s| s.cable_id).unwrap_or(u32::MAX));
        Ok(devices)
    }
}
pub fn delay(id: u32, frames: u32) -> Result<(), String> {
    if !(1..=8).contains(&id) || !(48..=480).contains(&frames) {
        return Err("ID must be 1..8 and delay 48..480 frames.".into());
    }
    let all = devices()?;
    let found = all
        .iter()
        .find(|d| d.status.as_ref().is_ok_and(|s| s.cable_id == id))
        .ok_or("Cable is not available. Run 'diagnose' to inspect installed devices.")?;
    let file = open(&found.path, true)?;
    let request = Delay {
        version: VERSION,
        size: 16,
        frames,
        reserved: 0,
    };
    let mut returned = 0;
    unsafe {
        check(
            DeviceIoControl(
                file.0,
                IOCTL_DELAY,
                &request as *const _ as _,
                16,
                null_mut(),
                0,
                &mut returned,
                null_mut(),
            ),
            "Set delay (both streams must be stopped)",
        )
    }
}
pub fn stage(path: &str) -> Result<String, String> {
    let path = std::fs::canonicalize(path).map_err(|e| format!("Driver INF: {e}"))?;
    if !path.is_file()
        || !path.file_name().is_some_and(|s| {
            s.to_str()
                .is_some_and(|s| s.eq_ignore_ascii_case("cableloom.inf"))
        })
    {
        return Err(
            "Select the cableloom.inf file from the signed Cableloom driver package.".into(),
        );
    }
    let text = path.to_str().ok_or("Driver path is not Unicode.")?;
    let normalized = if let Some(rest) = text.strip_prefix(r"\\?\UNC\") {
        format!(r"\\{rest}")
    } else {
        text.strip_prefix(r"\\?\").unwrap_or(text).to_string()
    };
    let path = wide(&normalized);
    unsafe {
        let mut destination = [0u16; 260];
        check(
            SetupCopyOEMInfW(
                path.as_ptr(),
                null(),
                1,
                0,
                destination.as_mut_ptr(),
                260,
                null_mut(),
                null_mut(),
            ),
            "Stage signed driver package (administrator access required)",
        )?;
        let end = destination.iter().position(|&v| v == 0).unwrap_or(260);
        Ok(format!(
            "Driver package staged as {}.",
            String::from_utf16_lossy(&destination[..end])
        ))
    }
}
fn instance(id: u32) -> Result<Vec<u16>, String> {
    if !(1..=8).contains(&id) {
        return Err("Cable ID must be 1..8.".into());
    }
    Ok(wide(&format!(r"ROOT\CABLELOOM\{id:04}")))
}
fn set_text(
    set: &DevSet,
    info: &DevInfo,
    property: u32,
    text: &str,
    multi: bool,
) -> Result<(), String> {
    let mut text = wide(text);
    if multi {
        text.push(0);
    }
    unsafe {
        check(
            SetupDiSetDeviceRegistryPropertyW(
                set.0,
                info,
                property,
                text.as_ptr() as _,
                (text.len() * 2) as u32,
            ),
            "Set cable device property",
        )
    }
}
pub fn add(id: u32) -> Result<String, String> {
    let name = instance(id)?;
    unsafe {
        let set = DevSet::new(SetupDiCreateDeviceInfoList(&MEDIA, null_mut()))?;
        let mut info = DevInfo::new();
        check(
            SetupDiCreateDeviceInfoW(
                set.0,
                name.as_ptr(),
                &MEDIA,
                wide(&format!("Cableloom {id}")).as_ptr(),
                null_mut(),
                0,
                &mut info,
            ),
            "Create cable device (ID may already exist; administrator access required)",
        )?;
        set_text(&set, &info, 1, r"Root\Cableloom", true)?;
        set_text(&set, &info, 12, &format!("Cableloom {id}"), false)?;
        set_text(&set, &info, 13, &id.to_string(), false)?;
        check(
            SetupDiCallClassInstaller(0x19, set.0, &info),
            "Register cable device",
        )?;
        let mut reboot = 0;
        if DiInstallDevice(null_mut(), set.0, &info, null(), 0, &mut reboot) == 0 {
            let original = error("Bind staged cable driver");
            let mut rollback_reboot = 0;
            if DiUninstallDevice(null_mut(), set.0, &info, 0, &mut rollback_reboot) == 0 {
                return Err(format!(
                    "{original}. Rollback failed: {}. Run 'remove {id}' to retry cleanup.",
                    std::io::Error::last_os_error()
                ));
            }
            return Err(format!(
                "{original}. New device was removed. Stage a signed package first.{}",
                if rollback_reboot != 0 {
                    " Restart Windows to finish cleanup."
                } else {
                    ""
                }
            ));
        }
        Ok(format!(
            "Cable {id} installed.{}",
            if reboot != 0 {
                " Restart Windows to finish installation."
            } else {
                ""
            }
        ))
    }
}
pub fn remove(id: u32) -> Result<String, String> {
    let name = instance(id)?;
    unsafe {
        let set = DevSet::new(SetupDiCreateDeviceInfoList(&MEDIA, null_mut()))?;
        let mut info = DevInfo::new();
        check(
            SetupDiOpenDeviceInfoW(set.0, name.as_ptr(), null_mut(), 0, &mut info),
            "Open installed cable",
        )?;
        let mut hardware = [0u16; 128];
        let mut kind = 0;
        check(
            SetupDiGetDeviceRegistryPropertyW(
                set.0,
                &info,
                1,
                &mut kind,
                hardware.as_mut_ptr() as _,
                256,
                null_mut(),
            ),
            "Verify cable hardware ID",
        )?;
        let end = hardware
            .iter()
            .position(|&v| v == 0)
            .ok_or("Invalid hardware ID.")?;
        if kind != 7
            || !String::from_utf16_lossy(&hardware[..end]).eq_ignore_ascii_case(r"Root\Cableloom")
        {
            return Err("Device hardware ID does not match Cableloom; removal refused.".into());
        }
        let mut reboot = 0;
        check(
            DiUninstallDevice(null_mut(), set.0, &info, 0, &mut reboot),
            "Remove cable (administrator access required)",
        )?;
        Ok(format!(
            "Cable {id} removed.{}",
            if reboot != 0 {
                " Restart Windows to finish removal."
            } else {
                ""
            }
        ))
    }
}
pub fn diagnose() -> Result<String, String> {
    unsafe {
        let set = DevSet::new(SetupDiGetClassDevsW(&MEDIA, null(), null_mut(), 0))?;
        let mut lines = Vec::new();
        for index in 0.. {
            let mut info = DevInfo::new();
            if SetupDiEnumDeviceInfo(set.0, index, &mut info) == 0 {
                if std::io::Error::last_os_error().raw_os_error() == Some(259) {
                    break;
                }
                return Err(error("Enumerate installed devices"));
            }
            let mut id = [0u16; 512];
            check(
                SetupDiGetDeviceInstanceIdW(set.0, &info, id.as_mut_ptr(), 512, null_mut()),
                "Read device instance ID",
            )?;
            let end = id.iter().position(|&v| v == 0).unwrap_or(512);
            let id = String::from_utf16_lossy(&id[..end]);
            if !id.to_ascii_uppercase().starts_with(r"ROOT\CABLELOOM\") {
                continue;
            }
            let mut status = 0;
            let mut problem = 0;
            let result = CM_Get_DevNode_Status(&mut status, &mut problem, info.instance, 0);
            lines.push(format!(
                "{id}: PnP status=0x{status:08x}, problem={problem}, query={result}"
            ));
        }
        if lines.is_empty() {
            Ok("No installed Cableloom devices. Stage a signed package, then add a cable.".into())
        } else {
            Ok(lines.join("\r\n"))
        }
    }
}
#[repr(C)]
struct ShellInfo {
    size: u32,
    mask: u32,
    hwnd: Handle,
    verb: *const u16,
    file: *const u16,
    parameters: *const u16,
    directory: *const u16,
    show: i32,
    instance: Handle,
    id_list: *mut c_void,
    class: *const u16,
    key: Handle,
    hotkey: u32,
    icon: Handle,
    process: Handle,
}
#[link(name = "shell32")]
extern "system" {
    fn ShellExecuteExW(info: *mut ShellInfo) -> i32;
}
pub fn quote_arg(arg: &str) -> String {
    let mut result = String::from("\"");
    let mut slashes = 0;
    for c in arg.chars() {
        if c == '\\' {
            slashes += 1;
            continue;
        }
        if c == '"' {
            result.extend(std::iter::repeat_n('\\', slashes * 2 + 1));
        } else {
            result.extend(std::iter::repeat_n('\\', slashes));
        }
        slashes = 0;
        result.push(c);
    }
    result.extend(std::iter::repeat_n('\\', slashes * 2));
    result.push('"');
    result
}
pub fn elevated(args: &[String]) -> Result<String, String> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let exe = wide(exe.to_str().ok_or("Executable path is not Unicode.")?);
    let verb = wide("runas");
    let parameters = wide(&format!(
        "--elevated {}",
        args.iter()
            .map(|a| quote_arg(a))
            .collect::<Vec<_>>()
            .join(" ")
    ));
    unsafe {
        let mut info: ShellInfo = zeroed();
        info.size = size_of::<ShellInfo>() as u32;
        info.mask = 0x140;
        info.verb = verb.as_ptr();
        info.file = exe.as_ptr();
        info.parameters = parameters.as_ptr();
        info.show = 1;
        check(ShellExecuteExW(&mut info), "Start administrator operation")?;
        if info.process.is_null() {
            return Err("Administrator helper did not return a process handle.".into());
        }
        let process = File(info.process);
        if WaitForSingleObject(process.0, u32::MAX) != 0 {
            return Err(error("Wait for administrator helper"));
        }
        let mut code = 0;
        check(
            GetExitCodeProcess(process.0, &mut code),
            "Read helper result",
        )?;
        if code != 0 {
            return Err(format!(
                "Administrator operation failed (exit {code}). The helper displays the error."
            ));
        }
        Ok(
            "Operation finished. Device enumeration may take a few seconds; refresh the list."
                .into(),
        )
    }
}

const _: () = assert!(size_of::<DevInfo>() == 32);
const _: () = assert!(size_of::<InterfaceInfo>() == 32);
const _: () = assert!(size_of::<ShellInfo>() == 112);

#[cfg(test)]
mod tests {
    use super::*;
    #[link(name = "shell32")]
    extern "system" {
        fn CommandLineToArgvW(line: *const u16, count: *mut i32) -> *mut *mut u16;
    }
    #[link(name = "kernel32")]
    extern "system" {
        fn LocalFree(memory: Handle) -> Handle;
    }
    #[test]
    fn quoted_arguments_roundtrip_through_windows_parser() {
        for text in [
            "",
            "plain",
            r"C:\plug ins\",
            "embedded\"quote",
            r#"ends\""#,
            "日本語",
            "%TEMP% & $data",
        ] {
            let command = wide(&format!("cableloom.exe {}", quote_arg(text)));
            unsafe {
                let mut count = 0;
                let pointers = CommandLineToArgvW(command.as_ptr(), &mut count);
                assert!(!pointers.is_null());
                let values = std::slice::from_raw_parts(pointers, count as usize);
                let mut received = Vec::new();
                for &value in values {
                    let mut length = 0;
                    while *value.add(length) != 0 {
                        length += 1;
                    }
                    received.push(
                        String::from_utf16(std::slice::from_raw_parts(value, length)).unwrap(),
                    );
                }
                LocalFree(pointers as Handle);
                assert_eq!(
                    received,
                    vec!["cableloom.exe".to_string(), text.to_string()]
                );
            }
        }
    }
}
