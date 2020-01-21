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

#ifdef ZTS
static MUTEX_T photon_agent_mutex = NULL;
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "ext/standard/php_string.h"
#include "php_photon.h"
#include "zend_extensions.h"
#include "Zend/zend_llist.h"
#include "SAPI.h"

// For compatibility with older PHP versions
#ifndef ZEND_PARSE_PARAMETERS_NONE
# define ZEND_PARSE_PARAMETERS_NONE() \
    ZEND_PARSE_PARAMETERS_START(0, 0) \
    ZEND_PARSE_PARAMETERS_END()
#endif

ZEND_DECLARE_MODULE_GLOBALS(photon)

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("photon.enable",              "1",                              PHP_INI_SYSTEM, OnUpdateBool,   enable,            zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.app_name",            "php-application",                PHP_INI_SYSTEM, OnUpdateString, app_name,          zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.app_version",         "0.1.0",                          PHP_INI_SYSTEM, OnUpdateString, app_version,       zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.agent_transport",     "udp",                            PHP_INI_SYSTEM, OnUpdateString, agent_transport,   zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.agent_host",          PHOTON_AGENT_DEFAULT_HOST,        PHP_INI_SYSTEM, OnUpdateString, agent_host,        zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.agent_port",          PHOTON_AGENT_DEFAULT_PORT,        PHP_INI_SYSTEM, OnUpdateLong,   agent_port,        zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.agent_socket_path",   PHOTON_AGENT_DEFAULT_SOCKET_PATH, PHP_INI_SYSTEM, OnUpdateString, agent_socket_path, zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.profiling_web",       "1",                              PHP_INI_SYSTEM, OnUpdateBool,   profiling_web,     zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.profiling_cli",       "1",                              PHP_INI_SYSTEM, OnUpdateBool,   profiling_cli,     zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.tracing_web",         "1",                              PHP_INI_SYSTEM, OnUpdateBool,   tracing_web,       zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.tracing_cli",         "1",                              PHP_INI_SYSTEM, OnUpdateBool,   tracing_cli,       zend_photon_globals, photon_globals)
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
    uint64_t diff = timespec_ns_diff(&start, &stop);
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

static zend_always_inline int extension_loaded(char *extension_name)
{
    char *lower_name = zend_str_tolower_dup(extension_name, strlen(extension_name));
    int result = zend_hash_str_exists(&module_registry, lower_name, strlen(lower_name));
    efree(lower_name);
    return result;
}

PHP_MINIT_FUNCTION(photon)
{
    REGISTER_INI_ENTRIES();

    PHOTON_G(transaction_log) = fopen("/tmp/photon-txn.log", "a");

    if (0 == PHOTON_G(enable)) {
        return SUCCESS;
    }

#ifdef ZTS
    photon_agent_mutex = tsrm_mutex_alloc();
#endif

    if (extension_loaded("PDO")) {
        printf("PDO is loaded\n");
    }
    if (extension_loaded("Redis")) {
        printf("Redis is loaded\n");
    }

    photon_connect_to_agent();
    photon_configure_interceptors();
    photon_override_execute();

    return SUCCESS;
}

static int photon_connect_to_agent()
{
#ifdef ZTS
    tsrm_mutex_lock(photon_agent_mutex);
#endif
    PHOTON_G(agent_connection) = pemalloc(sizeof(struct agent_connection), 1);

    // TODO: Close & free on shutdown!
    // TODO: On error - disable extension?
    // TODO: Refactor: too much similar code

    if (strcmp(PHOTON_G(agent_transport), "tcp") == 0) {
        // TODO: Handle errors
        int sd;
        struct sockaddr_in addr;
        // TODO: Replace with `getaddrinfo`, see https://www.kutukupret.com/2009/09/28/gethostbyname-vs-getaddrinfo/
        // TODO: Put pointer into agent connection structure and free at MSHUTDOWN
        struct hostent *hostname = gethostbyname(PHOTON_G(agent_host));

        addr.sin_family = AF_INET;
        addr.sin_port = htons(PHOTON_G(agent_port));
        addr.sin_addr.s_addr = *((unsigned long *)hostname->h_addr);
        sd = socket(AF_INET, SOCK_STREAM, 0);
        connect(sd, (struct sockaddr*)&addr, sizeof(addr));

        PHOTON_G(agent_connection)->sd = sd;
        PHOTON_G(agent_connection)->addr_in = addr;

#ifdef ZTS
        tsrm_mutex_unlock(photon_agent_mutex);
#endif
        return SUCCESS;
    } else if (strcmp(PHOTON_G(agent_transport), "udp") == 0) {
        // TODO: Handle errors
        int sd;
        struct sockaddr_in addr;

        addr.sin_family = AF_INET;
        addr.sin_port = htons(PHOTON_G(agent_port));
        addr.sin_addr.s_addr = inet_addr(PHOTON_G(agent_host));
        sd = socket(AF_INET, SOCK_DGRAM, 0);

        PHOTON_G(agent_connection)->sd = sd;
        PHOTON_G(agent_connection)->addr_in = addr;

#ifdef ZTS
        tsrm_mutex_unlock(photon_agent_mutex);
#endif
        return SUCCESS;
    } else if (strcmp(PHOTON_G(agent_transport), "unix") == 0) {
        // TODO: Handle errors
        int sd;
        struct sockaddr_un addr;

        addr.sun_family = AF_UNIX;
        sd = socket(PF_UNIX, SOCK_STREAM, 0);
        memset(&addr, 0, sizeof(addr));
        strcpy(addr.sun_path, PHOTON_G(agent_socket_path));
        // For SUN_LEN see https://unix.superglobalmegacorp.com/Net2/newsrc/sys/un.h.html
        connect(sd, (struct sockaddr*)&addr, SUN_LEN(&addr));

        PHOTON_G(agent_connection)->sd = sd;
        PHOTON_G(agent_connection)->addr_un = addr;

#ifdef ZTS
        tsrm_mutex_unlock(photon_agent_mutex);
#endif
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

    // TODO: Release interceptors, cleanup tracing / profiling structures
    photon_restore_execute();
    photon_disconnect_from_agent();

    fclose(PHOTON_G(transaction_log));

    return SUCCESS;
}

static int photon_disconnect_from_agent()
{
    // TODO: Is this condition needed?
    if (0 == PHOTON_G(enable)) {
        return SUCCESS;
    }

    close(PHOTON_G(agent_connection)->sd);

    if (strcmp(PHOTON_G(agent_transport), "tcp") == 0) {
        // TODO: Anything specific here?
    } else if (strcmp(PHOTON_G(agent_transport), "udp") == 0) {
        // TODO: Anything specific here?
    } else if (strcmp(PHOTON_G(agent_transport), "unix") == 0) {
        // TODO: Anything specific here?
    }

    // TODO: Do we need to take care of sockaddr inside?
    pefree(PHOTON_G(agent_connection), 1);

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

    // TODO: Move to `photon_transactions_list_init()` + `photon_transaction_start()`
    // Watch out: element is a pointer to transaction, not a transaction itself
    zend_llist *tl = &PHOTON_G(transactions_list);
    zend_llist_init(tl, sizeof(struct transaction *), (llist_dtor_func_t) photon_transaction_llist_element_dtor, 0);
    struct transaction *t = emalloc(sizeof(struct transaction));
    // TODO: Initial name?
    photon_transaction_ctor(t, "whatever", NULL);
    // Pass pointer to pointer
    zend_llist_add_element(tl, &t);

    return SUCCESS;
}

void photon_transaction_llist_element_dtor(struct transaction **tp)
{
    struct transaction *t = *tp;

    efree(t->app_name);
    efree(t->app_version);
    efree(t->endpoint_mode);
    efree(t->endpoint_name);
}

static int photon_transaction_ctor(struct transaction *t, char *endpoint_name, struct transaction *parent)
{
    // Transaction ID
    uuid_t uuid;
    uuid_generate_random(uuid);
    uuid_unparse_lower(uuid, t->id);

    if (NULL == parent) {
        // Note: trying to modify values read from config will lead to `zend_mm_heap corrupted`.
        t->app_name = estrdup(PHOTON_G(app_name));
        t->app_version = estrdup(PHOTON_G(app_version));
    } else {
        t->app_name = estrdup(parent->app_name);
        t->app_version = estrdup(parent->app_version);
    }

    // TODO: Use passed data (name, parent)
    // Mode (web or cli)
    if (
        strcmp(sapi_module.name, "fpm-fcgi") == 0 ||
        strcmp(sapi_module.name, "cli-server") == 0 ||
        strcmp(sapi_module.name, "cgi-fcgi") == 0 ||
        strcmp(sapi_module.name, "apache") == 0
    ) {
        // TODO: Should we add host name to transaction data?
        t->endpoint_name = estrdup(SG(request_info).request_uri);
        // TODO: Use enum instead?
        t->endpoint_mode = estrdup("web");
    } else if (strcmp(sapi_module.name, "cli") == 0) {
        // TODO: This is a full file path, resolved in `php_cli.c`. Just script filename should be enough
        t->endpoint_name = estrdup(SG(request_info).path_translated);
        // TODO: Use enum instead?
        t->endpoint_mode = estrdup("cli");
    }

    // Timestamp, monotonic timer, CPU timer
    clock_gettime(CLOCK_REALTIME, &(t->timestamp_start));
    clock_gettime(CLOCK_MONOTONIC, &(t->monotonic_timer_start));
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &(t->cpu_timer_start));

    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(photon)
{
    if (0 == PHOTON_G(enable)) {
        return SUCCESS;
    }

    // TODO: Main issue: can not properly pop tail, some issues with casting
    // TODO: Free profiling / tracing info
    zend_llist *tl = &PHOTON_G(transactions_list);
    while (0 < zend_llist_count(tl)) {
        // `data` is a double pointer
        struct transaction *t = (struct transaction *)*((struct transaction **)zend_llist_get_last(tl));
        photon_transaction_end(t);
        zend_llist_remove_tail(tl);
    }

    // TODO: Is immediate fflush required? Or is it enough to do it here, per-request?
    fflush(PHOTON_G(transaction_log));

    return SUCCESS;
}

// TODO: Return positive number: length of list
static int photon_transaction_end(struct transaction *t)
{
    // TODO: Is this condition needed?
    if (0 == PHOTON_G(enable)) {
        return SUCCESS;
    }

    printf("Ending transaction %s\n", t->id);

    struct timespec monotonic_timer_end;
    clock_gettime(CLOCK_MONOTONIC, &monotonic_timer_end);
    uint64_t monotonic_time_elapsed = timespec_ns_diff(&(t->monotonic_timer_start), &monotonic_timer_end);

    struct timespec cpu_timer_end;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_timer_end);
    uint64_t cpu_time_elapsed = timespec_ns_diff(&(t->cpu_timer_start), &cpu_timer_end);

    uint64_t timestamp = timespec_to_ns(&(t->timestamp_start));

    // See http://www.phpinternalsbook.com/php7/internal_types/strings/printing_functions.html
    char *result;
    int length;

    // TODO: Spaces in strings must be escaped
    length = spprintf(
        &result, 0,
        // `i` postfix is used in InfluxDB to force integer
        "txn,app=%s,ver=%s,mode=%s,endpoint=%s id=%s,tt=%"PRIu64"i,tc=%"PRIu64"i,mu=%lui,mr=%lui %"PRIu64"\n",
        t->app_name,
        t->app_version,
        // TODO: Use enum instead (reverse map into string)?
        t->endpoint_mode,
        t->endpoint_name,
        t->id,
        monotonic_time_elapsed,
        cpu_time_elapsed,
        zend_memory_peak_usage(0),
        zend_memory_peak_usage(1),
        timestamp
    );

//    int rc = photon_send_to_agent(result, length);
    fwrite(result, 1, length, PHOTON_G(transaction_log));

    efree(result);

    return SUCCESS;
}

static int photon_send_to_agent(char *data, size_t length)
{
    // TODO: Is this condition needed?
    if (0 == PHOTON_G(enable)) {
        return -1;
    }

    // Note that sender may buffer data in order to optimize performance.
    // We're okay with it, agent is capable of splitting messages.
    // See https://stackoverflow.com/questions/8421848/send-data-in-separate-tcp-segments-without-being-merged-by-the-tcp-stack

    if (strcmp(PHOTON_G(agent_transport), "udp") == 0) {
        // TODO: Handle error
        return sendto(
            PHOTON_G(agent_connection)->sd,
            data,
            length + 1,
            0,
            (struct sockaddr*)&PHOTON_G(agent_connection)->addr_in,
            sizeof(PHOTON_G(agent_connection)->addr_in)
        );
    }

    // UNIX and TCP have identical `send`
    // TODO: Handle error
    return send(PHOTON_G(agent_connection)->sd, data, length, 0);
}

PHP_FUNCTION(photon_get_app_name)
{
    RETURN_TRUE;
//    RETURN_STRING(PHOTON_G(current_transaction)->app_name);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_photon_set_app_name, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(photon_set_app_name)
{
    zend_string *name = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_STR_EX(name, 1, 0)
    ZEND_PARSE_PARAMETERS_END();

    if (ZSTR_LEN(name) == 0) {
        RETURN_FALSE;
    }

    // TODO: Should we check for null?
//    struct transaction *ct = PHOTON_G(current_transaction);
//    efree(ct->app_name);
//    ct->app_name = estrdup(ZSTR_VAL(name));
//    zend_string_release(name);

    RETURN_TRUE;
}

PHP_FUNCTION(photon_get_app_version)
{
    RETURN_TRUE;
//    RETURN_STRING(PHOTON_G(current_transaction)->app_version);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_photon_set_app_version, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, version, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(photon_set_app_version)
{
    zend_string *version = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_STR_EX(version, 1, 0)
    ZEND_PARSE_PARAMETERS_END();

    if (ZSTR_LEN(version) == 0) {
        RETURN_FALSE;
    }

//    // TODO: Should we check for null?
//    struct transaction *ct = PHOTON_G(current_transaction);
//    efree(ct->app_version);
//    ct->app_version = estrdup(ZSTR_VAL(version));
//    zend_string_release(version);

    RETURN_TRUE;
}

PHP_FUNCTION(photon_get_endpoint_name)
{
    RETURN_TRUE;
//    RETURN_STRING(PHOTON_G(current_transaction)->endpoint_name);
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

//    // TODO: Should we check for null?
//    struct transaction *ct = PHOTON_G(current_transaction);
//    efree(ct->endpoint_name);
//    ct->endpoint_name = estrdup(ZSTR_VAL(name));
//    zend_string_release(name);

    RETURN_TRUE;
}

PHP_FUNCTION(photon_get_transaction_id)
{
    RETURN_TRUE;
//    RETURN_STRING(PHOTON_G(current_transaction)->id);
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

    // TODO: Set it

    RETURN_TRUE;
}

/**
 * A list of extension's functions exposed to developers.
 */
static const zend_function_entry photon_functions[] = {
    PHP_FE(photon_get_app_name,      NULL)
    PHP_FE(photon_set_app_name,      arginfo_photon_set_app_name)
    PHP_FE(photon_get_app_version,   NULL)
    PHP_FE(photon_set_app_version,   arginfo_photon_set_app_version)
    PHP_FE(photon_get_endpoint_name, NULL)
    PHP_FE(photon_set_endpoint_name, arginfo_photon_set_endpoint_name)
    PHP_FE(photon_get_trace_id,      NULL)
    PHP_FE(photon_set_trace_id,      arginfo_photon_set_trace_id)
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
