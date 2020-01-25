# PHP extension for Photon

This extension collects data for [Photon](https://github.com/kozlice/photon).

## Installation

Extension depends on shared `libuuid`, so you need it installed:
```bash
# Debian
apt-get install libuuid1
# CentOS
yum install libuuid
```

#### Building from source

Install dependencies:

```bash
# Debian
apt-get install uuid-dev
# CentOS
yum install libuuid-devel
```

Clone & build:

```bash
git clone https://github.com/kozlice/photon-php-ext
cd photon-php-ext
phpize --clean
phpize
./configure
make
make install
```

## Configuration

All values in these example are the default values.

```ini
; Overall extension switch.
photon.enable = 1;

; Default application info. Can be modified at runtime using
; functions provided by the extension.
photon.app_name = "app";
photon.app_version = "0.1.0";
photon.app_env = "dev";

; Path to transaction log.
photon.transaction_log_path = "/var/log/photon-php/transaction.log";

; Whether to enable or not profiling in web and CLI.
photon.profiling_enable = 1;
photon.profiling_enable_cli = 1;

; What fraction of requests should be profiled. Can be a float,
; e.g. `5.8%`.
photon.profiling_sample_freq = 5%;
```
