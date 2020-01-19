
## Plan

### PHP extension
+ Learn how to create basic extension with functions
+ Learn how to hook into functions/methods execution
- Decide with sampling modes: random sampling vs time-based sampling vs every nth
- Configuration
    
    ```
    ; Overall extension behaviour (on or off).
    ; Default is 1.
    photon.enable = 1;
    
    ; Your application's name and version.
    ; These can be changed during runtime.
    photon.application_name = "PHP application";
    photon.application_version = "0.1.0";
    
    ; It is possible to send data to agent over
    ; UDP, TCP, or Unix socket.
    photon.agent_transport = "udp";
    photon.agent_socket_path = "/var/run/photon-agent.sock";
    photon.agent_host = "localhost";
    photon.agent_port = 8989;
    
    ; Whether to enable or not profiling in web
    ; (
    photon.profiling_web = 1;
    photon.profiling_cli = 1;
    
    ;
    photon.profiling_mode = always | sampling | off;
    
    ; If mode is "sampling", take every N requests out of 100.
    ; Otherwise this parameter is ignored.
    ; Default is 5.
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
    
- Forced inline for execution interceptors, or use macros
- Add dependency on UUID (`libuuid`) and use it for request ID generation / parsing.
  See https://stackoverflow.com/questions/51053568/generating-a-random-uuid-in-c
- Override functions
    - curl (what about `curl_exec_multi`?)
    - PDO
    - Redis
    - mysqli
    - Memcache & Memcached
    - MongoDB
- Override userland functions
    - AMQP/RabbitMQ
    - Elasticsearch
    - Guzzle
    - PRedis
- Split into multiple files, otherwise .h and .c will be unreadable

### Agent
[ ] Get data from UNIX socket (or TCP socket) and proxy it to server

### Server
[ ] Setup application, allow agents to connect over TCP and send reports
[ ] Write down measurements structure for InfluxDB
[ ] Plan continuous queries to save space (store summaries rather than raw data)
[ ] Plan API for frontend

### Frontend / UI
[ ] Plan pages (charts and tables) and navigation between pages
