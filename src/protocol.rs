pub const VERSION: u32 = 1;
pub const IOCTL_STATUS: u32 = 0x0022_6000;
pub const IOCTL_DELAY: u32 = 0x0022_a004;
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct Request { pub version: u32, pub size: u32 }
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct Delay { pub version: u32, pub size: u32, pub frames: u32, pub reserved: u32 }
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct Status {
    pub version: u32, pub size: u32, pub cable_id: u32, pub delay_frames: u32,
    pub sample_rate: u32, pub channels: u32, pub bits: u32, pub running: u32,
    pub render_frames: u64, pub capture_frames: u64, pub underrun_frames: u64, pub overrun_frames: u64,
    pub discontinuities: u64, pub timer_ticks: u64,
}
const _: () = assert!(std::mem::size_of::<Request>() == 8);
const _: () = assert!(std::mem::size_of::<Delay>() == 16);
const _: () = assert!(std::mem::size_of::<Status>() == 80);
impl Status {
    pub fn validate(&self) -> Result<(), String> {
        if self.version != VERSION || self.size != 80 || !(1..=8).contains(&self.cable_id) ||
            self.sample_rate != 48000 || self.channels != 2 || !matches!(self.bits,16|24|32) ||
            !(48..=480).contains(&self.delay_frames) || self.running & !3 != 0 {
            Err("Driver returned an incompatible status record.".into())
        } else { Ok(()) }
    }
    pub fn json(&self) -> String {
        format!(concat!("{{\"id\":{},\"delay_frames\":{},\"sample_rate\":{},\"channels\":{},\"bits\":{},",
            "\"running\":{},\"render_frames\":{},\"capture_frames\":{},\"underrun_frames\":{},",
            "\"overrun_frames\":{},\"discontinuities\":{},\"timer_ticks\":{}}}"),
            self.cable_id, self.delay_frames, self.sample_rate, self.channels, self.bits, self.running,
            self.render_frames, self.capture_frames, self.underrun_frames, self.overrun_frames,
            self.discontinuities, self.timer_ticks)
    }
    pub fn line(&self) -> String {
        format!("Cable {} | delay {} frames ({:.2} ms) | {} Hz / {} ch / PCM{} | render {} capture {}\r\n  Rendered {}  Captured {}  Missing {}  Dropped {}  Discontinuities {}",
            self.cable_id, self.delay_frames, self.delay_frames as f64 / 48.0, self.sample_rate,
            self.channels, self.bits, self.running & 1 != 0, self.running & 2 != 0,
            self.render_frames, self.capture_frames, self.underrun_frames, self.overrun_frames, self.discontinuities)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn status_rejects_unsupported_wire_values() {
        let mut s = Status { version: VERSION, size:80, cable_id:1, delay_frames:96, sample_rate:48000, channels:2, bits:32, ..Status::default() };
        assert!(s.validate().is_ok());
        s.size = 79; assert!(s.validate().is_err()); s.size = 80;
        s.running = 4; assert!(s.validate().is_err()); s.running = 0;
        s.cable_id = 9; assert!(s.validate().is_err());
    }
}
