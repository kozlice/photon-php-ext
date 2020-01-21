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
#include "Zend/zend_stack.h"
#include "SAPI.h"
#include "php_photon.h"

// For compatibility with older PHP versions
#ifndef ZEND_PARSE_PARAMETERS_NONE
#define ZEND_PARSE_PARAMETERS_NONE() \
    ZEND_PARSE_PARAMETERS_START(0, 0) \
    ZEND_PARSE_PARAMETERS_END()
#endif

ZEND_DECLARE_MODULE_GLOBALS(photon)

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("photon.enable",               "1",                   PHP_INI_SYSTEM, OnUpdateBool,   enable,               zend_photon_globals, photon_globals)
    // TODO: Change default path to `/var/log/photon-php-transactions.log` or something like that
    STD_PHP_INI_ENTRY("photon.transaction_log_path", "/tmp/photon-txn.log", PHP_INI_SYSTEM, OnUpdateString, transaction_log_path, zend_photon_globals, photon_globals)
PHP_INI_END()

// Returns clock as u64 instead of structure
static uint64_t clock_gettime_as_ns(clockid_t clk_id)
{
    struct timespec t;
    clock_gettime(clk_id, &t);

    return t.tv_sec * 1e9 + t.tv_nsec;
}

// Checks if module/extension is loaded
static zend_always_inline int extension_loaded(char *extension_name)
{
    char *lower_name = zend_str_tolower_dup(extension_name, strlen(extension_name));
    int result = zend_hash_str_exists(&module_registry, lower_name, strlen(lower_name));
    efree(lower_name);

    return result;
}

// Storage for original VM execution functions
static void (*original_zend_execute_ex)(zend_execute_data *execute_data);
static void (*original_zend_execute_internal)(zend_execute_data *execute_data, zval *return_value);

// Execution interceptor
ZEND_API zend_always_inline void photon_execute_base(char internal, zend_execute_data *execute_data, zval *return_value)
{
    // TODO: Check if function needs to be intercepted

    if (internal) {
        if (original_zend_execute_internal) {
            original_zend_execute_internal(execute_data, return_value);
        } else {
            execute_internal(execute_data, return_value);
        }
    } else {
        original_zend_execute_ex(execute_data);
    }

    // TODO: ...
}

// Wrapper for userland function calls
ZEND_API void photon_execute_ex(zend_execute_data *execute_data)
{
    photon_execute_base(0, execute_data, NULL);
}

// Wrapper for internal function calls
ZEND_API void photon_execute_internal(zend_execute_data *execute_data, zval *return_value)
{
    photon_execute_base(1, execute_data, return_value);
}

PHP_MINIT_FUNCTION(photon)
{
    // Read configuration
    REGISTER_INI_ENTRIES();

    if (PHOTON_NOT_ENABLED) {
        return SUCCESS;
    }

    // Open transaction log
    // TODO: If log opening failed, log error, disable extension & return early
    PHOTON_TXN_LOG = fopen(PHOTON_G(transaction_log_path), "a");

    // Userland constants
    REGISTER_STRING_CONSTANT("PHOTON_TXN_PROPERTY_APP_NAME", "app_name", CONST_CS | CONST_PERSISTENT);
    // TODO: The rest of them

    // TODO: Init interceptors

    // Overload VM execution functions
    original_zend_execute_internal = zend_execute_internal;
    original_zend_execute_ex = zend_execute_ex;
    zend_execute_internal = photon_execute_internal;
    zend_execute_ex = photon_execute_ex;

    return SUCCESS;
}

PHP_MINFO_FUNCTION(photon)
{
    // TODO: Print actual settings:
    php_info_print_table_start();
    php_info_print_table_header(2, "photon support", "enabled");
    php_info_print_table_row(2, "photon version", PHP_PHOTON_VERSION);
    php_info_print_table_row(2, "transaction log path", PHOTON_G(transaction_log_path));
    php_info_print_table_end();
}

PHP_MSHUTDOWN_FUNCTION(photon)
{
    if (PHOTON_NOT_ENABLED) {
        return SUCCESS;
    }

    // Restore original VM execution functions
    zend_execute_internal = original_zend_execute_internal;
    zend_execute_ex = original_zend_execute_ex;

    // TODO: Release interceptors

    // Close transaction log
    fclose(PHOTON_TXN_LOG);

    // Release configuration
    UNREGISTER_INI_ENTRIES();

    return SUCCESS;
}

PHP_RINIT_FUNCTION(photon)
{
#if defined(ZTS) && defined(COMPILE_DL_PHOTON)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif

    if (PHOTON_NOT_ENABLED) {
        return SUCCESS;
    }

    // Initialize transaction stack
    PHOTON_TXN_STACK = emalloc(sizeof(zend_stack));
    zend_stack_init(PHOTON_TXN_STACK, sizeof(transaction *));

    // Create root transaction
    photon_txn_start(NULL);

    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(photon)
{
    if (PHOTON_NOT_ENABLED) {
        return SUCCESS;
    }

    // Drain transaction stack: will end all pending transactions
    while (!zend_stack_is_empty(PHOTON_TXN_STACK)) {
        photon_txn_end();
    }

    zend_stack_destroy(PHOTON_TXN_STACK);

    // Force writing transaction entries to disk. Doing it here can save some IO
    fflush(PHOTON_TXN_LOG);

    return SUCCESS;
}

static char *photon_get_default_endpoint_name()
{
    if (0 == strcmp(sapi_module.name, "cli")) {
        return *SG(request_info).argv;
    }

    return SG(request_info).request_uri;
}

static zend_always_inline transaction *photon_get_current_txn()
{
    transaction **tp = zend_stack_top(PHOTON_TXN_STACK);

    if (NULL == tp) {
        return NULL;
    }

    return *tp;
}

static void photon_txn_start(char *endpoint_name)
{
    transaction *next = emalloc(sizeof(transaction));
    transaction *prev = photon_get_current_txn();

    // Generate ID
    // See https://stackoverflow.com/questions/51053568/generating-a-random-uuid-in-c
    uuid_t uuid;
    uuid_generate_random(uuid);
    uuid_unparse_lower(uuid, next->id);

    // Measure time
    next->timestamp = clock_gettime_as_ns(CLOCK_REALTIME);
    next->timer_monotonic = clock_gettime_as_ns(CLOCK_MONOTONIC);
    next->timer_cpu = clock_gettime_as_ns(CLOCK_THREAD_CPUTIME_ID);

    // TODO: If prev exists, copy properties. Otherwise this is a root transaction, resolve from environment
    if (NULL != endpoint_name) {
        next->endpoint_name = estrdup(endpoint_name);
    } else {
        next->endpoint_name = estrdup(photon_get_default_endpoint_name());
    }

    if (NULL != prev) {
        next->app_name = estrdup(prev->app_name);
        next->app_version = estrdup(prev->app_version);
    } else {
        // TODO: Defaults?
        next->app_name = estrdup("app");
        next->app_version = estrdup("0.0.1");
    }

    // Push result onto transaction stack (watch out: double pointer here)
    zend_stack_push(PHOTON_TXN_STACK, &next);
}

static void photon_txn_dtor(transaction *txn)
{
    efree(txn->app_name);
    efree(txn->app_version);
    efree(txn->endpoint_name);
}

static void photon_txn_end()
{
    transaction *txn = photon_get_current_txn();

    // Build report line
    // See http://www.phpinternalsbook.com/php7/internal_types/strings/printing_functions.html
    // TODO: Output all data
    char *data;
    int length = spprintf(
        &data, 0,
        "%s %s@%s %s %s %"PRIu64" %"PRIu64" %zu %zu %"PRIu64"\n",
        txn->id,
        txn->app_name,
        txn->app_version,
        sapi_module.name,
        txn->endpoint_name,
        clock_gettime_as_ns(CLOCK_MONOTONIC) - txn->timer_monotonic,
        clock_gettime_as_ns(CLOCK_THREAD_CPUTIME_ID) - txn->timer_cpu,
        zend_memory_usage(0),
        zend_memory_usage(1),
        txn->timestamp
    );

    // Write into log file
    fwrite(data, 1, length, PHOTON_TXN_LOG);

    // Remove transaction from stack & destroy
    zend_stack_del_top(PHOTON_TXN_STACK);
    photon_txn_dtor(txn);
}

static const zend_function_entry photon_functions[] = {
    PHP_FE_END
};

static const zend_module_dep photon_deps[] = {
    ZEND_MOD_REQUIRED("curl")
    ZEND_MOD_END
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
