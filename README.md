# PHP extension for Photon

This extension gathers data and ships it to [Photon](https://github.com/kozlice/photon).

## Installation

#### Compiling from source
```bash
phpize --clean
phpize
./configure
make
make install
```

## Configuration
```ini
; Overall extension behaviour (on or off).
; Default is 1.
photon.enable = 1;

; Your application's name and version.
; These can be changed during runtime.
photon.application_name = "PHP application";
photon.application_version = "0.1.0";

; It is possible to send data to agent over UDP, TCP, or Unix socket.
photon.agent_transport = "udp";
photon.agent_socket_path = "/var/run/photon-agent.sock";
photon.agent_host = "localhost";
photon.agent_port = 8989;

; Whether to enable or not profiling in web and CLI.
photon.profiling_web = 1;
photon.profiling_cli = 1;

; TODO: Profiling modes & options description
photon.profiling_mode = always | sampling | off;

photon.profiling_sampling_rate = 5;

; Enables HTTP tracing: outgoing requests will have an
; additional HTTP header, and incoming request with such
; header will change current request's trace ID.
; Default is 1.
photon.tracing_http = 1;

; HTTP header name for the setting above.
; Default to "X-Photon-Trace-Id".
photon.tracing_http_header_name = "X-Photon-Trace-Id";

; Enables AMQP tracing: outgoing messages will have an
; additional header, and incoming messages will change
; current request's trace ID.
; Default is 1.
photon.tracing_amqp = 1;

; AMQP header name for the setting above.
; Default to "x_photon_trace_id".
photon.tracing_amqp_header_name = "x_photon_trace_id";
```
