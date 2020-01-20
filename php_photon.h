/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author: Valentin Nazarov                                             |
   +----------------------------------------------------------------------+
*/

#ifndef PHP_PHOTON_H
# define PHP_PHOTON_H

// TODO: Only keep what we need
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <inttypes.h>

#include <uuid/uuid.h>

extern zend_module_entry photon_module_entry;
# define phpext_photon_ptr &photon_module_entry

# define PHP_PHOTON_NAME "photon"
# define PHP_PHOTON_VERSION "0.1.0"

# if defined(ZTS) && defined(COMPILE_DL_PHOTON)
ZEND_TSRMLS_CACHE_EXTERN()
# endif

# define PHOTON_AGENT_DEFAULT_HOST "localhost"
// During .ini parsing it will be converted into long, but input must be string
# define PHOTON_AGENT_DEFAULT_PORT "8989"
# define PHOTON_AGENT_DEFAULT_SOCKET_PATH "/var/run/photon-agent.sock"

// TODO: Macros for intercepting functions and class methods
// TODO: Structs for holding request basic info and detailed report

ZEND_BEGIN_MODULE_GLOBALS(photon)
    zend_bool enable;

    zend_bool profiling_web;
    zend_bool profiling_cli;

    zend_bool tracing_web;
    zend_bool tracing_cli;

    char *application_name;
    char *application_version;

    char *current_application_name;
    char *current_application_version;
    char *current_endpoint_name;
    char *current_mode;
    struct timespec current_request_start_time;
    struct timespec current_request_start_cpu_clock;

    // TODO: Socket connection itself: need a union for TCP/UDP/Unix
    char *agent_transport;
    char *agent_host;
    long  agent_port;
    char *agent_socket_path;

    // TODO: Request stats (memory, CPU & time, trace ID)
    // TODO: Profiling stack/log (class+function, stack depth, execution time + tags)
    // TODO: This is temporary, need to move to profiling
    int stack_depth;
ZEND_END_MODULE_GLOBALS(photon)

// Define globals accessor
#ifdef ZTS
#define PHOTON_G(v) TSRMG(photon_globals_id, photon_globals *, v)
#else
#define PHOTON_G(v) (photon_globals.v)
#endif

ZEND_API zend_always_inline void photon_execute_base(char internal, zend_execute_data *execute_data, zval *return_value);
ZEND_API void photon_execute_internal(zend_execute_data *execute_data, zval *return_value);
ZEND_API void photon_execute_ex (zend_execute_data *execute_data);

static int photon_minit_connect_to_agent();
static int photon_minit_configure_interceptors();
static int photon_minit_override_execute();

static int photon_mshutdown_disconnect_from_agent();
static int photon_mshutdown_restore_execute();

PHP_MINIT_FUNCTION(photon);
PHP_MSHUTDOWN_FUNCTION(photon);
PHP_MINFO_FUNCTION(photon);

PHP_RINIT_FUNCTION(photon);
PHP_RSHUTDOWN_FUNCTION(photon);

PHP_FUNCTION(photon_get_application_name);
PHP_FUNCTION(photon_set_application_name);
PHP_FUNCTION(photon_get_application_version);
PHP_FUNCTION(photon_set_application_version);
PHP_FUNCTION(photon_get_endpoint_name);
PHP_FUNCTION(photon_set_endpoint_name);
PHP_FUNCTION(photon_get_trace_id);
PHP_FUNCTION(photon_set_trace_id);

#endif /* PHP_PHOTON_H */
