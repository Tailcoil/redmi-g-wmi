# redmi-g-wmi

Redmi G 2022（锐龙版，主板 TM2137）的 WMI 驱动，替代内核自带的 `bitland-mifs-wmi` 和 `redmi-wmi`。

- 4 档性能模式，和 Fn+K 同步
- 风扇转速、CPU 温度
- 触控板锁、Fn 锁、Win 键锁
- 热键

## 安装

需要 `dkms` 和当前内核的头文件。

```sh
sudo dkms add ./driver
sudo dkms install redmi-g-wmi/0.1.0
printf 'blacklist bitland_mifs_wmi\nblacklist redmi_wmi\n' | sudo tee /etc/modprobe.d/redmi-g-wmi.conf
sudo modprobe -r bitland_mifs_wmi redmi_wmi
sudo modprobe redmi-g-wmi
```

卸载：

```sh
sudo dkms remove redmi-g-wmi/0.1.0 --all
sudo rm /etc/modprobe.d/redmi-g-wmi.conf
```

## 使用

### 性能模式

| platform_profile | BIOS 档位 | 条件 |
|---|---|---|
| `low-power` | 安静 | |
| `balanced` | 均衡 | |
| `balanced-performance` | 性能 | DC 适配器 > 180W |
| `performance` | 狂暴 | DC 适配器 > 230W |

条件不满足时 BIOS 会降档。用电池时 `balanced-performance` 和 `performance` 降为 `balanced`，写入返回错误。

```sh
cat /sys/firmware/acpi/platform_profile
echo balanced-performance | sudo tee /sys/firmware/acpi/platform_profile
```

power-profiles-daemon 没有 `balanced-performance`，只能用上面的方式或 Fn+K 切换。

### 传感器

```sh
sensors redmi_g_wmi-virtual-0
```

### 锁

```sh
cd /sys/bus/wmi/devices/B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B-*
echo 1 | sudo tee touchpad_lock
```

| 属性 | 1 |
|---|---|
| `touchpad_lock` | 触控板禁用 |
| `fn_lock` | F1–F12 默认是功能键 |
| `win_lock` | Win 键禁用 |

## 控制台

`console/` 是网页控制台：切换模式、开关锁、实时查看温度、风扇、功率限制和每个核心的频率电压。只监听本机，默认 `http://127.0.0.1:8930`。

```sh
cargo build --release --manifest-path console/Cargo.toml
sudo install -Dm755 console/target/release/redmi-g-console /usr/local/bin/redmi-g-console
sudo install -Dm644 console/redmi-g-console.service /etc/systemd/system/redmi-g-console.service
sudo systemctl enable --now redmi-g-console
```

功率限制和核心数据来自 [ryzen_smu](https://github.com/amkillam/ryzen_smu)，适配器功率要 root 和 `ec_sys`，缺了对应部分就不显示。本机所有用户都能访问控制台。

## 目录

- `driver/`：内核模块
- `upstream/`：上游原版代码（Linux v7.2.9），用来对照
- `console/`：网页控制台
- `tools/`：BIOS 接口探测脚本

## 许可证

`driver/` 含 redmi-wmi 的代码，按 GPL-2.0-only 发布；其余部分是 GPL-2.0-or-later。
