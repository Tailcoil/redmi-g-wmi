//! 读写驱动和其他内核接口。路径每次重新查找，驱动重载后编号变了也不受影响。

use std::fs::{self, File};
use std::io;
use std::os::unix::fs::FileExt;
use std::path::{Path, PathBuf};

use serde::Serialize;

const WMI_CONTROL: &str = "B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B";
const PM_TABLE: &str = "/sys/kernel/ryzen_smu_drv";
const PM_TABLE_VERSION: u32 = 0x0045_0005;
const EC_IO: &str = "/sys/kernel/debug/ec/ec0/io";
const EC_ADAPTER_WATTS: u64 = 0x8E;

pub const LOCKS: [&str; 3] = ["touchpad_lock", "fn_lock", "win_lock"];

#[derive(Serialize)]
pub struct Snapshot {
    profile: Option<String>,
    ac: Option<bool>,
    adapter_w: Option<u32>,
    battery: Option<Battery>,
    fans: [Option<u32>; 2],
    cpu_temp: Option<f32>,
    temp2: Option<f32>,
    locks: Option<Locks>,
    smu: Option<Smu>,
}

#[derive(Serialize)]
struct Battery {
    capacity: u32,
    status: String,
    power_w: f32,
}

#[derive(Serialize)]
struct Locks {
    touchpad: bool,
    r#fn: bool,
    win: bool,
}

#[derive(Serialize)]
struct Smu {
    limits: Vec<Limit>,
    cores: Vec<Core>,
}

#[derive(Serialize)]
struct Limit {
    name: &'static str,
    value: f32,
    limit: f32,
}

#[derive(Serialize)]
struct Core {
    freq: f32,
    volt: f32,
    temp: f32,
    power: f32,
}

pub fn snapshot() -> Snapshot {
    let hwmon = find_class("hwmon", "name", "redmi_g_wmi");
    let hwmon_val = |file: &str| {
        hwmon
            .as_deref()
            .and_then(|d| read_num::<i64>(&d.join(file)))
    };
    let temp = |file| hwmon_val(file).map(|v| v as f32 / 1000.0);

    Snapshot {
        profile: profile_dir().and_then(|d| read_str(&d.join("profile"))),
        ac: find_class("power_supply", "type", "Mains")
            .and_then(|d| read_num::<u8>(&d.join("online")))
            .map(|v| v == 1),
        adapter_w: adapter_watts(),
        battery: battery(),
        fans: [hwmon_val("fan1_input"), hwmon_val("fan2_input")].map(|v| v.map(|v| v as u32)),
        cpu_temp: temp("temp1_input"),
        temp2: temp("temp2_input"),
        locks: locks(),
        smu: smu(),
    }
}

pub fn set_profile(name: &str) -> io::Result<()> {
    let dir = profile_dir().ok_or(io::ErrorKind::NotFound)?;
    let choices = read_str(&dir.join("choices")).unwrap_or_default();
    if !choices.split(' ').any(|c| c == name) {
        return Err(io::ErrorKind::InvalidInput.into());
    }
    fs::write(dir.join("profile"), name)
}

pub fn set_lock(name: &str, on: bool) -> io::Result<()> {
    if !LOCKS.contains(&name) {
        return Err(io::ErrorKind::InvalidInput.into());
    }
    let dir = wmi_dir().ok_or(io::ErrorKind::NotFound)?;
    fs::write(dir.join(name), if on { "1" } else { "0" })
}

fn profile_dir() -> Option<PathBuf> {
    find_class("platform-profile", "name", "redmi-g-wmi")
}

fn wmi_dir() -> Option<PathBuf> {
    fs::read_dir("/sys/bus/wmi/devices")
        .ok()?
        .flatten()
        .find(|e| e.file_name().to_string_lossy().starts_with(WMI_CONTROL))
        .map(|e| e.path())
}

fn locks() -> Option<Locks> {
    let dir = wmi_dir()?;
    let get = |name| read_num::<u8>(&dir.join(name)).map(|v| v == 1);
    Some(Locks {
        touchpad: get(LOCKS[0])?,
        r#fn: get(LOCKS[1])?,
        win: get(LOCKS[2])?,
    })
}

fn battery() -> Option<Battery> {
    let dir = find_class("power_supply", "type", "Battery")?;
    Some(Battery {
        capacity: read_num(&dir.join("capacity"))?,
        status: read_str(&dir.join("status"))?,
        power_w: read_num::<u64>(&dir.join("power_now"))? as f32 / 1e6,
    })
}

/// EC 里的适配器额定功率，需要 ec_sys 和 root
fn adapter_watts() -> Option<u32> {
    let mut buf = [0u8; 2];
    File::open(EC_IO)
        .ok()?
        .read_exact_at(&mut buf, EC_ADAPTER_WATTS)
        .ok()?;
    Some(u16::from_be_bytes(buf) as u32).filter(|&w| w > 0)
}

/// ryzen_smu 的 PM 表，布局只按 0x450005（Rembrandt）解析
fn smu() -> Option<Smu> {
    let dir = Path::new(PM_TABLE);
    let ver = fs::read(dir.join("pm_table_version")).ok()?;
    if u32::from_le_bytes(ver.get(..4)?.try_into().ok()?) != PM_TABLE_VERSION {
        return None;
    }
    let raw = fs::read(dir.join("pm_table")).ok()?;
    let f: Vec<f32> = raw
        .as_chunks()
        .0
        .iter()
        .map(|b| f32::from_le_bytes(*b))
        .collect();
    if f.len() < 684 {
        return None;
    }

    // 开头是 (限值, 当前值) 对
    let pair = |name, i: usize| Limit {
        name,
        limit: f[i],
        value: f[i + 1],
    };
    let limits = vec![
        pair("STAPM", 0),
        pair("PPT Fast", 2),
        pair("PPT Slow", 4),
        pair("TDC", 8),
        pair("EDC", 12),
        pair("Tctl", 16),
    ];
    let cores = (0..8)
        .map(|k| Core {
            power: f[219 + k],
            volt: f[227 + k],
            temp: f[235 + k],
            freq: f[267 + k],
        })
        .collect();
    Some(Smu { limits, cores })
}

/// 在 /sys/class/<class> 下找 <attr> 等于 <value> 的设备
fn find_class(class: &str, attr: &str, value: &str) -> Option<PathBuf> {
    fs::read_dir(Path::new("/sys/class").join(class))
        .ok()?
        .flatten()
        .map(|e| e.path())
        .find(|p| read_str(&p.join(attr)).as_deref() == Some(value))
}

fn read_str(path: &Path) -> Option<String> {
    fs::read_to_string(path)
        .ok()
        .map(|s| s.trim_end().to_owned())
}

fn read_num<T: std::str::FromStr>(path: &Path) -> Option<T> {
    read_str(path)?.parse().ok()
}
