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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "php_photon.h"
#include "zend_extensions.h"
#include "SAPI.h"

// For compatibility with older PHP versions
#ifndef ZEND_PARSE_PARAMETERS_NONE
# define ZEND_PARSE_PARAMETERS_NONE() \
    ZEND_PARSE_PARAMETERS_START(0, 0) \
    ZEND_PARSE_PARAMETERS_END()
#endif

ZEND_DECLARE_MODULE_GLOBALS(photon)

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("photon.enable",              "1",                              PHP_INI_SYSTEM, OnUpdateBool,   enable,              zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.application_name",    "php-application",                PHP_INI_SYSTEM, OnUpdateString, application_name,    zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.application_version", "0.1.0",                          PHP_INI_SYSTEM, OnUpdateString, application_version, zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.agent_transport",     "udp",                            PHP_INI_SYSTEM, OnUpdateString, agent_transport,     zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.agent_host",          PHOTON_AGENT_DEFAULT_HOST,        PHP_INI_SYSTEM, OnUpdateString, agent_host,          zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.agent_port",          PHOTON_AGENT_DEFAULT_PORT,        PHP_INI_SYSTEM, OnUpdateLong,   agent_port,          zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.agent_socket_path",   PHOTON_AGENT_DEFAULT_SOCKET_PATH, PHP_INI_SYSTEM, OnUpdateString, agent_socket_path,   zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.profiling_web",       "1",                              PHP_INI_SYSTEM, OnUpdateBool,   profiling_web,       zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.profiling_cli",       "1",                              PHP_INI_SYSTEM, OnUpdateBool,   profiling_cli,       zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.tracing_web",         "1",                              PHP_INI_SYSTEM, OnUpdateBool,   tracing_web,         zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.tracing_cli",         "1",                              PHP_INI_SYSTEM, OnUpdateBool,   tracing_cli,         zend_photon_globals, photon_globals)
    // TODO: Profiling and tracing options
PHP_INI_END()

static zend_always_inline uint64_t timespec_ns_diff(struct timespec *start, struct timespec *end)
{
    return timespec_to_ns(end) - timespec_to_ns(start);
}

static zend_always_inline uint64_t timespec_to_ns(struct timespec *ts)
{
    return ts->tv_sec * 1e9 + ts->tv_nsec;
}

static void (*original_zend_execute_ex)(zend_execute_data *execute_data);
static void (*original_zend_execute_internal)(zend_execute_data *execute_data, zval *return_value);

ZEND_API zend_always_inline void photon_execute_base(char internal, zend_execute_data *execute_data, zval *return_value)
{
    zend_function *zf = execute_data->func;
    const char *class_name = (zf->common.scope != NULL && zf->common.scope->name != NULL) ? ZSTR_VAL(zf->common.scope->name) : NULL;
    const char *function_name = zf->common.function_name == NULL ? NULL : ZSTR_VAL(zf->common.function_name);
    // TODO: Profiling depends on this: internal functions do not have op_array, different output (use smart str?)
    const char *file_name = internal ? NULL : ZSTR_VAL(zf->op_array.filename);

    PHOTON_G(stack_depth)++;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // TODO: Need to do this within a try-catch and bailout in case of error
    // TODO: This is not optimal, I would rather avoid checking for `internal` every time. Use macros instead or inline pre/post?
    if (internal) {
        if (original_zend_execute_internal) {
            original_zend_execute_internal(execute_data, return_value);
        } else {
            execute_internal(execute_data, return_value);
        }
    } else {
        original_zend_execute_ex(execute_data);
    }

    struct timespec stop;
    clock_gettime(CLOCK_MONOTONIC, &stop);
    double diff = (stop.tv_sec - start.tv_sec) * 1e3 + (stop.tv_nsec - start.tv_nsec) / 1e6;
    //printf("%d - %s - %s@%s - %f ms\n", PHOTON_G(stack_depth), file_name, class_name, function_name, diff);
    PHOTON_G(stack_depth)--;
}

ZEND_API void photon_execute_ex(zend_execute_data *execute_data)
{
    photon_execute_base(0, execute_data, NULL);
}

ZEND_API void photon_execute_internal(zend_execute_data *execute_data, zval *return_value)
{
    photon_execute_base(1, execute_data, return_value);
}

static int extension_loaded(char *extension_name)
{
    return zend_hash_str_exists(&module_registry, extension_name, strlen(extension_name));
}

PHP_MINIT_FUNCTION(photon)
{
    REGISTER_INI_ENTRIES();

    if (0 == PHOTON_G(enable)) {
        return SUCCESS;
    }

    photon_connect_to_agent();
    photon_configure_interceptors();
    photon_override_execute();

    return SUCCESS;
}

static int photon_connect_to_agent()
{
    // TODO: Close & free on shutdown!
    // TODO: Create socket connection: must be per-process, not per-thread, yet synced write into buffer is required
    // TODO: On error - disable extension?

    if (strcmp(PHOTON_G(agent_transport), "tcp") == 0) {
        // TODO: Handle errors

        // TODO: Establish connection

        return SUCCESS;
    } else if (strcmp(PHOTON_G(agent_transport), "udp") == 0) {
        // TODO: Handle errors
        int sd;
        struct sockaddr_in addr;

        sd = socket(AF_INET, SOCK_DGRAM, 0);
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PHOTON_G(agent_port));
        addr.sin_addr.s_addr = inet_addr(PHOTON_G(agent_host));

        PHOTON_G(agent_connection).sd = sd;
        PHOTON_G(agent_connection).addr_in = addr;

//        printf("Socket: %d, host: %s, port: %d\n", PHOTON_G(agent_connection).sd, inet_ntoa(PHOTON_G(agent_connection).addr_in.sin_addr), PHOTON_G(agent_connection).addr_in.sin_port);

        return SUCCESS;
    } else if (strcmp(PHOTON_G(agent_transport), "unix") == 0) {
        // TODO: Handle errors
        int sd;
        struct sockaddr_un addr;

        sd = socket(PF_UNIX, SOCK_STREAM, 0);
        addr.sun_family = AF_UNIX;
        memset(&addr, 0, sizeof(addr));
        strcpy(addr.sun_path, PHOTON_G(agent_socket_path));
        connect(sd, (struct sockaddr*)&addr, SUN_LEN(&addr));

        PHOTON_G(agent_connection).sd = sd;
        PHOTON_G(agent_connection).addr_un = addr;

        return SUCCESS;
    }

    // TODO: Log unknown transport & disable extension
    return FAILURE;
}

static int photon_configure_interceptors()
{
    // TODO: Configure interceptors for internal & userland functions
    return SUCCESS;
}

static int photon_override_execute()
{
    // Overload VM execution functions. This allows custom tracing/profiling
    original_zend_execute_internal = zend_execute_internal;
    original_zend_execute_ex = zend_execute_ex;
    zend_execute_internal = photon_execute_internal;
    zend_execute_ex = photon_execute_ex;

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(photon)
{
    UNREGISTER_INI_ENTRIES();

    if (0 == PHOTON_G(enable)) {
        return SUCCESS;
    }

    // TODO: Free memory - release interceptors, cleanup all other stuff
    photon_restore_execute();
    photon_disconnect_from_agent();

    return SUCCESS;
}

static int photon_disconnect_from_agent()
{
    // TODO: Is this condition needed?
    if (0 == PHOTON_G(enable)) {
        return SUCCESS;
    }

    if (strcmp(PHOTON_G(agent_transport), "tcp") == 0) {
        // TODO: What here?
    } else if (strcmp(PHOTON_G(agent_transport), "udp") == 0) {
        // TODO: What here?
    } else if (strcmp(PHOTON_G(agent_transport), "unix") == 0) {
        close(PHOTON_G(agent_connection).sd);
    }

    return SUCCESS;
}

static int photon_restore_execute()
{
    zend_execute_internal = original_zend_execute_internal;
    zend_execute_ex = original_zend_execute_ex;

    return SUCCESS;
}

PHP_MINFO_FUNCTION(photon)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "photon support", "enabled");
    php_info_print_table_row(2, "photon version", PHP_PHOTON_VERSION);
    php_info_print_table_row(2, "agent transport", PHOTON_G(agent_transport));
    // TODO: Print all other settings; some are config-dependent (transport -> path or host:port)
    php_info_print_table_end();
}

PHP_RINIT_FUNCTION(photon)
{
#if defined(ZTS) && defined(COMPILE_DL_PHOTON)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif

    if (0 == PHOTON_G(enable)) {
        return SUCCESS;
    }

    // TODO: Check SAPI name: profiling & tracing for web & CLI depending on config
    // TODO: Capture request start time and transaction name (URI or filename)
    // TODO: Read or create X-Photon-Trace-Id, will be used for tracing
    // TODO: Init span stack
    // TODO: Split into functions

    // To make runtime changes possible, duplicate these. Otherwise trying to `efree`
    // the original leads to `zend_mm_heap corrupted`.
    PHOTON_G(current_application_name) = estrdup(PHOTON_G(application_name));
    PHOTON_G(current_application_version) = estrdup(PHOTON_G(application_version));

    if (
        strcmp(sapi_module.name, "fpm-fcgi") == 0 ||
        strcmp(sapi_module.name, "cli-server") == 0 ||
        strcmp(sapi_module.name, "cgi-fcgi") == 0 ||
        strcmp(sapi_module.name, "apache") == 0
    ) {
        // TODO: Should we add host name to transaction data?
        PHOTON_G(current_endpoint_name) = estrdup(SG(request_info).request_uri);
        PHOTON_G(current_mode) = estrdup("web");
    } else if (strcmp(sapi_module.name, "cli") == 0) {
        // TODO: This is a full file path, resolved in `php_cli.c`. Just script filename should be enough
        PHOTON_G(current_endpoint_name) = estrdup(SG(request_info).path_translated);
        PHOTON_G(current_mode) = estrdup("cli");
    }

    clock_gettime(CLOCK_REALTIME, &PHOTON_G(current_request_timestamp_start));
    clock_gettime(CLOCK_MONOTONIC, &PHOTON_G(current_request_timer_start));
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &PHOTON_G(current_request_cpu_timer_start));

    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(photon)
{
    if (0 == PHOTON_G(enable)) {
        return SUCCESS;
    }

    // TODO: Free profiling / tracing info
    photon_report_transaction_info();

    return SUCCESS;
}

static int photon_report_transaction_info()
{
    // TODO: Is this condition needed?
    if (0 == PHOTON_G(enable)) {
        return SUCCESS;
    }

    struct timespec current_request_timer_end;
    clock_gettime(CLOCK_MONOTONIC, &current_request_timer_end);
    uint64_t current_request_duration = timespec_ns_diff(&PHOTON_G(current_request_timer_start), &current_request_timer_end);

    struct timespec current_request_cpu_timer_end;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &current_request_cpu_timer_end);
    uint64_t current_request_cpu_time = timespec_ns_diff(&PHOTON_G(current_request_cpu_timer_start), &current_request_cpu_timer_end);

    uint64_t timestamp = timespec_to_ns(&PHOTON_G(current_request_timestamp_start));

    // See http://www.phpinternalsbook.com/php7/internal_types/strings/printing_functions.html
    char *result;
    int length;

    // TODO: Spaces in strings must be escaped
    length = spprintf(
        &result, 0,
        "txn,app=%s,ver=%s,mode=%s,endpoint=%s tt=%"PRIu64"i,tc=%"PRIu64"i,mu=%lu,mr=%lu %"PRIu64,
        PHOTON_G(current_application_name),
        PHOTON_G(current_application_version),
        PHOTON_G(current_mode),
        PHOTON_G(current_endpoint_name),
        current_request_duration,
        current_request_cpu_time,
        zend_memory_peak_usage(0),
        zend_memory_peak_usage(1),
        timestamp
    );

    int rc = photon_send_to_agent(result, length);

    efree(result);

    return SUCCESS;
}

static int photon_send_to_agent(char *data, size_t length)
{
    // TODO: Is this condition needed?
    if (0 == PHOTON_G(enable)) {
        return -1;
    }

    if (strcmp(PHOTON_G(agent_transport), "tcp") == 0) {
        // TODO: ?

        return 0;
    }

    if (strcmp(PHOTON_G(agent_transport), "udp") == 0) {
        // TODO: Handle error
        return sendto(
            PHOTON_G(agent_connection).sd,
            data,
            length + 1,
            0,
            (struct sockaddr*)&PHOTON_G(agent_connection).addr_in,
            sizeof(PHOTON_G(agent_connection).addr_in)
        );
    }

    if (strcmp(PHOTON_G(agent_transport), "unix") == 0) {
        // TODO: Handle error
        return send(PHOTON_G(agent_connection).sd, data, length, 0);
    }

    return -1;
}

PHP_FUNCTION(photon_get_application_name)
{
    RETURN_STRING(PHOTON_G(current_application_name));
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_photon_set_application_name, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(photon_set_application_name)
{
    zend_string *name = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_STR_EX(name, 1, 0)
    ZEND_PARSE_PARAMETERS_END();

    if (ZSTR_LEN(name) == 0) {
        RETURN_FALSE;
    }

    // TODO: This approach is used in built-in extensions, so I assume it's memory safe
    if (PHOTON_G(current_application_name)) {
        efree(PHOTON_G(current_application_name));
    }
    PHOTON_G(current_application_name) = estrdup(ZSTR_VAL(name));
    zend_string_release(name);

    RETURN_TRUE;
}

PHP_FUNCTION(photon_get_application_version)
{
    RETURN_STRING(PHOTON_G(current_application_version));
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_photon_set_application_version, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, version, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(photon_set_application_version)
{
    zend_string *version = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_STR_EX(version, 1, 0)
    ZEND_PARSE_PARAMETERS_END();

    if (ZSTR_LEN(version) == 0) {
        RETURN_FALSE;
    }

    // TODO: This approach is used in built-in extensions, so I assume it's memory safe
    if (PHOTON_G(current_application_version)) {
        efree(PHOTON_G(current_application_version));
    }
    PHOTON_G(current_application_version) = estrdup(ZSTR_VAL(version));
    zend_string_release(version);

    RETURN_TRUE;
}

PHP_FUNCTION(photon_get_endpoint_name)
{
    RETURN_STRING(PHOTON_G(current_endpoint_name));
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_photon_set_endpoint_name, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(photon_set_endpoint_name)
{
    zend_string *name = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_STR_EX(name, 1, 0)
    ZEND_PARSE_PARAMETERS_END();

    // TODO: This approach is used in built-in extensions, so I assume it's memory safe
    if (PHOTON_G(current_endpoint_name)) {
        efree(PHOTON_G(current_endpoint_name));
    }
    PHOTON_G(current_endpoint_name) = estrdup(ZSTR_VAL(name));
    zend_string_release(name);

    RETURN_TRUE;
}

PHP_FUNCTION(photon_get_trace_id)
{
    // TODO: Return a string from UUID
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_photon_set_trace_id, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, id, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(photon_set_trace_id)
{
    zend_string *id = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_STR_EX(id, 1, 0)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_TRUE;
}

/**
 * A list of extension's functions exposed to developers.
 */
static const zend_function_entry photon_functions[] = {
    PHP_FE(photon_get_application_name,    NULL)
    PHP_FE(photon_set_application_name,    arginfo_photon_set_application_name)
    PHP_FE(photon_get_application_version, NULL)
    PHP_FE(photon_set_application_version, arginfo_photon_set_application_version)
    PHP_FE(photon_get_endpoint_name,    NULL)
    PHP_FE(photon_set_endpoint_name,    arginfo_photon_set_endpoint_name)
    PHP_FE(photon_get_trace_id,            NULL)
    PHP_FE(photon_set_trace_id,            arginfo_photon_set_trace_id)
    PHP_FE_END
};

static const zend_module_dep molten_deps[] = {
    // TODO: Declare all dependencies, maybe using `HAS_X`: pdo, memcache, memcached, redis, amqp (?)
    ZEND_MOD_REQUIRED("curl")
};

zend_module_entry photon_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_PHOTON_NAME,                  /* Extension name */
    photon_functions,                 /* List of functions */
    PHP_MINIT(photon),                /* PHP_MINIT - Module initialization */
    PHP_MSHUTDOWN(photon),            /* PHP_MSHUTDOWN - Module shutdown */
    PHP_RINIT(photon),                /* PHP_RINIT - Request initialization */
    PHP_RSHUTDOWN(photon),            /* PHP_RSHUTDOWN - Request shutdown */
    PHP_MINFO(photon),                /* PHP_MINFO - Module info */
    PHP_PHOTON_VERSION,               /* Extension version */
    STANDARD_MODULE_PROPERTIES        /* TODO: See if we need to define the rest fields */
};

#ifdef COMPILE_DL_PHOTON
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(photon)
#endif
