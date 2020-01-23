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

; Path to transaction log.
photon.transaction_log_path = "/var/log/photon-php-transaction.log";

; Whether to enable or not profiling in web and CLI.
photon.profiling_web = 1;
photon.profiling_cli = 1;

; TODO: Profiling modes & options description
photon.profiling_mode = "always" | "sampling" | "off";

photon.profiling_sampling_rate = 5;

; Enables HTTP tracing: outgoing requests will have an
; additional HTTP header, and incoming request with such
; header will change current request's trace ID.
photon.tracing_http = 1;

; HTTP header name for the setting above.
photon.tracing_http_header_name = "X-Photon-Trace-Id";

; Enables AMQP tracing: outgoing messages will have an
; additional header, and incoming messages will change
; current request's trace ID.
photon.tracing_amqp = 1;

; AMQP header name for the setting above.
photon.tracing_amqp_header_name = "x_photon_trace_id";
```
