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
#include "Zend/zend_ptr_stack.h"
#include "Zend/zend_smart_string.h"
#include "SAPI.h"
#include "php_photon.h"

// For compatibility with older PHP versions
#ifndef ZEND_PARSE_PARAMETERS_NONE
#define ZEND_PARSE_PARAMETERS_NONE() \
    ZEND_PARSE_PARAMETERS_START(0, 0) \
    ZEND_PARSE_PARAMETERS_END()
#endif

ZEND_DECLARE_MODULE_GLOBALS(photon)

static PHP_INI_MH(OnUpdateProfilingSampleFreq)
{
    double percent;

    percent = atof(ZSTR_VAL(new_value));
    percent = percent < 0.0 ? 0.0 : percent;
    percent = percent > 100.0 ? 100.0 : percent;

    PHOTON_G(profiling_sample_freq) = percent;

    return SUCCESS;
}

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("photon.enable",                "1",                   PHP_INI_SYSTEM, OnUpdateBool, enable, zend_photon_globals, photon_globals)

    // TODO: Change default path to `/var/log/photon-php/transactions.log` or something like that
    // TODO: Custom updater?
    STD_PHP_INI_ENTRY("photon.transaction_log_path",  "/tmp/photon-txn.log", PHP_INI_SYSTEM, OnUpdateString, transaction_log_path, zend_photon_globals, photon_globals)

    STD_PHP_INI_ENTRY("photon.profiling_enable",      "1",                   PHP_INI_SYSTEM, OnUpdateBool, profiling_enable,     zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.profiling_enable_cli",  "1",                   PHP_INI_SYSTEM, OnUpdateBool, profiling_enable_cli, zend_photon_globals, photon_globals)

    STD_PHP_INI_ENTRY("photon.profiling_sample_freq", "5%",                  PHP_INI_SYSTEM, OnUpdateProfilingSampleFreq, profiling_sample_freq, zend_photon_globals, photon_globals)
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

// Will store original VM execution functions
static void (*original_zend_execute_ex)(zend_execute_data *execute_data);
static void (*original_zend_execute_internal)(zend_execute_data *execute_data, zval *return_value);

// Execution interceptor
ZEND_API static zend_always_inline void photon_execute_base(char internal, zend_execute_data *execute_data, zval *return_value)
{
    // TODO: See if we should do something about closures and generators
    // TODO: Also, check if traits renaming makes sense
    zend_function *zf = execute_data->func;

    const char *class_name = (zf->common.scope != NULL && zf->common.scope->name != NULL) ? ZSTR_VAL(zf->common.scope->name) : NULL;
    const char *function_name = zf->common.function_name == NULL ? NULL : ZSTR_VAL(zf->common.function_name);

    // Build interceptor key
    // Use `smart_string`: it has `char *` as internal storage, while `smart_str` uses `zend_string`
    smart_string itc_name = {0};

    if (NULL != class_name) {
        // +1 is for separator between class & method name
        smart_string_appends(&itc_name, class_name);
        smart_string_appendc(&itc_name, PHOTON_ITC_SEPARATOR);
    }

    if (NULL != function_name) {
        smart_string_appends(&itc_name, function_name);
    }

    smart_string_0(&itc_name);
    interceptor *itc = NULL;

    if (itc_name.len) {
        interceptor **itc_ptr = zend_hash_str_find_ptr(PHOTON_INTERCEPTORS, itc_name.c, itc_name.len);
        if (NULL != itc_ptr) {
            itc = *itc_ptr;
            // TODO: Split callback into before & after
            if (NULL != itc && NULL != itc->fn) {
                itc->fn(execute_data);
            }
        }
    }

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

    // Do not free function & class name - they are owned by execute_data
    smart_string_free(&itc_name);
}

// Wrapper for userland function calls
ZEND_API static void photon_execute_ex(zend_execute_data *execute_data)
{
    photon_execute_base(0, execute_data, NULL);
}

// Wrapper for internal function calls
ZEND_API static void photon_execute_internal(zend_execute_data *execute_data, zval *return_value)
{
    photon_execute_base(1, execute_data, return_value);
}

// TODO: This is just an example interceptor
void curl_exec_interceptor_callback(zend_execute_data *execute_data)
{
    puts("curl_exec called");
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

    // Init interceptors
    // TODO: See if we can make it work faster by using a custom hash function
    PHOTON_INTERCEPTORS = pemalloc(sizeof(HashTable), 1);
    zend_hash_init(PHOTON_INTERCEPTORS, 128, NULL, photon_interceptor_dtor, 1);

    // TODO: Define actual interceptors
    if (extension_loaded("curl")) {
        photon_interceptor_add("curl_exec", &curl_exec_interceptor_callback);
    }

    if (extension_loaded("PDO")) {

    }

    if (extension_loaded("mysqli")) {

    }

    if (extension_loaded("pgsql")) {

    }

    if (extension_loaded("redis")) {

    }

    if (extension_loaded("memcache")) {

    }

    if (extension_loaded("memcached")) {

    }

    // Overload VM execution functions
    original_zend_execute_internal = zend_execute_internal;
    original_zend_execute_ex = zend_execute_ex;
    zend_execute_internal = photon_execute_internal;
    zend_execute_ex = photon_execute_ex;

    return SUCCESS;
}

PHP_MINFO_FUNCTION(photon)
{
    // TODO: Print actual settings
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

    // Release interceptors: elements will be passed to `photon_interceptor_dtor`, so only free the hashtable itself
    pefree(PHOTON_INTERCEPTORS, 1);

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
    PHOTON_TXN_STACK = emalloc(sizeof(zend_ptr_stack));
    zend_ptr_stack_init(PHOTON_TXN_STACK);

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
    while (zend_ptr_stack_num_elements(PHOTON_TXN_STACK)) {
        photon_txn_end();
    }

    zend_ptr_stack_destroy(PHOTON_TXN_STACK);
    efree(PHOTON_TXN_STACK);

    // Force writing transaction entries to disk. Doing it here can save some IO
    fflush(PHOTON_TXN_LOG);

    return SUCCESS;
}

static void photon_interceptor_add(char *name, interceptor_handler fn)
{
    interceptor *itc = (interceptor *) pemalloc(sizeof(interceptor *), 1);
    itc->name = pestrdup(name, 1);
    itc->fn = fn;
    zend_hash_str_add_mem(PHOTON_INTERCEPTORS, name, strlen(name), &itc, sizeof(interceptor *));
}

static void photon_interceptor_dtor(zval *entry)
{
    // HashTable calls destructor for defined interceptors, so this can't be null
    interceptor *itc = *(interceptor **)entry;
    pefree(itc->name, 1);
    pefree(itc, 1);
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
    if (zend_ptr_stack_num_elements(PHOTON_TXN_STACK)) {
        return zend_ptr_stack_top(PHOTON_TXN_STACK);
    }

    return NULL;
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

    // TODO: Decide on profiling: copy parent's value or do random number according to rate

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

    // Push pointer to the transaction onto stack
    zend_ptr_stack_push(PHOTON_TXN_STACK, (void *)next);
}

static void photon_txn_dtor(transaction *txn)
{
    efree(txn->id);
    efree(txn->app_name);
    efree(txn->app_version);
    efree(txn->endpoint_name);
}

static void photon_txn_end()
{
    transaction *txn = zend_ptr_stack_pop(PHOTON_TXN_STACK);

    // TODO: If this is root transaction, we can add `SG(sapi_headers).http_response_code)` to report (non-CLI)
    // TODO: Also, there is `EG(exit_status)`. When exception is thrown, it's 255 (under any SAPI), and `exit(N)` works

    // Build report line
    // See http://www.phpinternalsbook.com/php7/internal_types/strings/printing_functions.html
    char *data;
    // TODO: This call allocates `smart_str` for the pattern, and it is lost, but cleaned up thanks to ZendMM
    // TODO: Need to quote strings and escape double quotes
    int length = spprintf(
        &data, 0,
        "%s,%s,%s,%s,%s,%"PRIu64",%"PRIu64",%zu,%zu,%"PRIu64"\n",
        txn->app_name,
        txn->app_version,
        sapi_module.name,
        txn->endpoint_name,
        txn->id,
        clock_gettime_as_ns(CLOCK_MONOTONIC) - txn->timer_monotonic,
        clock_gettime_as_ns(CLOCK_THREAD_CPUTIME_ID) - txn->timer_cpu,
        zend_memory_usage(0),
        zend_memory_usage(1),
        txn->timestamp
    );

    // Write into log file
    fwrite(data, 1, length, PHOTON_TXN_LOG);

    photon_txn_dtor(txn);
    efree(txn);
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
    PHP_MINIT(photon),                /* PHP_MINIT - module initialization */
    PHP_MSHUTDOWN(photon),            /* PHP_MSHUTDOWN - module shutdown */
    PHP_RINIT(photon),                /* PHP_RINIT - request initialization */
    PHP_RSHUTDOWN(photon),            /* PHP_RSHUTDOWN - request shutdown */
    PHP_MINFO(photon),                /* PHP_MINFO - module info */
    PHP_PHOTON_VERSION,               /* Extension version */
    PHP_MODULE_GLOBALS(photon),       /* Got globals */
    NULL,                             /* No PHP_GINIT */
    NULL,                             /* No PHP_GSHUTDOWN */
    NULL,                             /* No post-deactivate function */
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_PHOTON
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(photon)
#endif
