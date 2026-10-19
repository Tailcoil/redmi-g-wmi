# redmi-g-wmi

Redmi G 2022（锐龙版，主板 TM2137）的 WMI 驱动。

从内核自带的 `bitland-mifs-wmi` 分叉而来，按这台机器的 BIOS 重新适配了功能号和性能模式。

## 构建

```sh
make -C driver
```

## 安装

```sh
sudo dkms add ./driver
sudo dkms install redmi-g-wmi/0.1.0
echo 'blacklist bitland_mifs_wmi' | sudo tee /etc/modprobe.d/redmi-g-wmi.conf
```

## 目录

- `driver/`：内核模块
- `upstream/`：上游原版代码（Linux v7.2.9），用来对照

## 许可证

GPL-2.0-or-later
