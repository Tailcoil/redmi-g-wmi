# redmi-g-wmi

Redmi G 2022（锐龙版，主板 TM2137）的 WMI 驱动。

合并并替代内核自带的 `bitland-mifs-wmi` 和 `redmi-wmi`，按这台机器的 BIOS 重新适配。

- 4 档性能模式：`low-power` / `balanced` / `performance` / `max-power`，和 Fn+K 同步
- 风扇转速、CPU 温度（hwmon）
- 触控板锁、Fn 锁、Win 键锁
- 热键

## 构建

```sh
make -C driver
```

## 安装

```sh
sudo dkms add ./driver
sudo dkms install redmi-g-wmi/0.1.0
printf 'blacklist bitland_mifs_wmi\nblacklist redmi_wmi\n' | sudo tee /etc/modprobe.d/redmi-g-wmi.conf
```

## 接口

```
/sys/firmware/acpi/platform_profile
/sys/bus/wmi/devices/B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B-*/{touchpad,fn,win}_lock
```

`fn_lock` 为 1 时 F1–F12 默认是功能键。

## 目录

- `driver/`：内核模块
- `upstream/`：上游原版代码（Linux v7.2.9），用来对照
- `tools/`：BIOS 接口探测脚本

## 许可证

GPL-2.0-or-later
