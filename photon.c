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
#include "ext/standard/php_random.h"
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

// TODO: Only support up to .01%?
static PHP_INI_MH(OnUpdateProfilerSamplingFreq)
{
    double percent;

    percent = atof(ZSTR_VAL(new_value));
    percent = percent < 0.0 ? 0.0 : percent;
    percent = percent > 100.0 ? 100.0 : percent;

    PHOTON_G(profiler_sampling_freq) = percent;
    return SUCCESS;
}

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("photon.enable",      "1",     PHP_INI_SYSTEM, OnUpdateBool, enable, zend_photon_globals, photon_globals)

    STD_PHP_INI_ENTRY("photon.app_name",    "app",   PHP_INI_SYSTEM, OnUpdateStringUnempty, app_name,    zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.app_version", "0.1.0", PHP_INI_SYSTEM, OnUpdateStringUnempty, app_version, zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.app_env",     "dev",   PHP_INI_SYSTEM, OnUpdateStringUnempty, app_env,     zend_photon_globals, photon_globals)

    STD_PHP_INI_ENTRY("photon.profiler_enable",              "1",  PHP_INI_SYSTEM, OnUpdateBool,                 profiler_enable,              zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.profiler_enable_cli",          "1",  PHP_INI_SYSTEM, OnUpdateBool,                 profiler_enable_cli,          zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.profiler_sampling_freq",       "5%", PHP_INI_SYSTEM, OnUpdateProfilerSamplingFreq, profiler_sampling_freq,       zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.profiler_trigger_http_header", "",   PHP_INI_SYSTEM, OnUpdateString,               profiler_trigger_http_header, zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.profiler_trigger_query_param", "",   PHP_INI_SYSTEM, OnUpdateString,               profiler_trigger_query_param, zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.profiler_trigger_cookie_name", "",   PHP_INI_SYSTEM, OnUpdateString,               profiler_trigger_cookie_name, zend_photon_globals, photon_globals)

    // TODO: Custom handlers?
    // TODO: Change default path to `/var/log/photon-php/transactions.log` and `/var/log/photon-php/profiler/`
    STD_PHP_INI_ENTRY("photon.transaction_log_path", "/tmp/photon/transactions.log", PHP_INI_SYSTEM, OnUpdateStringUnempty, transaction_log_path, zend_photon_globals, photon_globals)
    STD_PHP_INI_ENTRY("photon.profiler_output_dir",  "/tmp/photon/profiler/",        PHP_INI_SYSTEM, OnUpdateStringUnempty, profiler_output_dir, zend_photon_globals, photon_globals)
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
    transaction *txn = photon_get_current_txn();
    txn->stack_depth++;

    zend_function *zf = execute_data->func;

    // TODO: Tests are required to check that everything is properly named in the profile result
    // TODO: - when method is called from parent class, the parent's name is shown, not the class itself
    // TODO: - when method from trait is called, classname is shown
    // TODO: - when method from trait is renamed (A::stuff as other), original name is still shown (stuff)
    const char *class_name = (zf->common.scope != NULL && zf->common.scope->name != NULL) ? ZSTR_VAL(zf->common.scope->name) : NULL;
    const char *function_name = zf->common.function_name == NULL ? NULL : ZSTR_VAL(zf->common.function_name);

    // Build interceptor key
    // Use `smart_string`: it has `char *` as internal storage, while `smart_str` uses `zend_string`
    smart_string itc_name = {0};

    if (NULL != class_name) {
        // +1 is for separator between class & method name
        smart_string_appends(&itc_name, class_name);
        smart_string_appends(&itc_name, PHOTON_ITC_SEPARATOR);
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
            // TODO: Split callback into before & after?
            if (NULL != itc && NULL != itc->fn) {
                itc->fn(execute_data);
            }
        }
    }

    profiler_span *span;
    uint64_t span_timer_monotonic;
    uint64_t span_timer_cpu;

    if (txn->profiler_enable) {
        span = emalloc(sizeof(profiler_span));

        // TODO: What should be used as name? `{main}` is an option, but still
        span->name = estrdup(itc_name.len ? itc_name.c : "{main}");
        span->stack_depth = txn->stack_depth;

        span_timer_monotonic = clock_gettime_as_ns(CLOCK_MONOTONIC_RAW);
        span_timer_cpu = clock_gettime_as_ns(CLOCK_THREAD_CPUTIME_ID);

        // Watch out: double pointers
        zend_llist_add_element(txn->profiler_spans, &span);
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

    if (txn->profiler_enable) {
        span->duration_monotonic = clock_gettime_as_ns(CLOCK_MONOTONIC_RAW) - span_timer_monotonic;
        span->duration_cpu = clock_gettime_as_ns(CLOCK_THREAD_CPUTIME_ID) - span_timer_cpu;
    }

    // TODO: ...

    // Do not free function & class name - they are owned by execute_data
    smart_string_free(&itc_name);

    txn->stack_depth--;
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
    // TODO: Disable extension or trigger failure?
    PHOTON_TXN_LOG = fopen(PHOTON_G(transaction_log_path), "a");
    if (NULL == PHOTON_TXN_LOG) {
        PHOTON_ERROR("[PHOTON] Unable to open transaction log for writing, code: %d, reason: %s\n", errno, strerror(errno));
        return FAILURE;
    }

    // If profiling is enabled, ensure directory
    // TODO: Disable extension or trigger failure?
    if (1 == PHOTON_G(profiler_enable) || 1 == PHOTON_G(profiler_enable_cli)) {
        if (0 != access(PHOTON_G(profiler_output_dir), W_OK)) {
            PHOTON_ERROR("[PHOTON] Unable to open profiling directory for writing, code: %d, reason: %s\n", errno, strerror(errno));
            return FAILURE;
        }
    }

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

static zend_bool photon_should_profile()
{
    if (0 == strcmp(sapi_module.name, "cli") && 0 == PHOTON_G(profiler_enable_cli)) {
        return 0;
    }

    if (0 == PHOTON_G(profiler_enable)) {
        return 0;
    }

    // TODO: Refactor this. Looks terrible
    zval *value = NULL;
    if (
        0 < strlen(PHOTON_G(profiler_trigger_http_header))
        && (
            Z_TYPE(PG(http_globals)[TRACK_VARS_SERVER]) == IS_ARRAY
            || zend_is_auto_global_str(ZEND_STRL("_SERVER"))
        )
        && (NULL != (value = zend_hash_str_find(Z_ARRVAL(PG(http_globals)[TRACK_VARS_SERVER]), "HTTP_X_PHOTON_ENABLE_PROFILING", strlen("HTTP_X_PHOTON_ENABLE_PROFILING"))))
        && IS_STRING == Z_TYPE_P(value)
        && 0 != Z_STRLEN_P(value)
    ) {
        // TODO: If value is truthy, return true (or simply present at all)
    }

    // TODO: Same for query param
    // TODO: Same for cookies

    zend_long dice;
    // TODO: This function can throw, need to handle that
    php_random_int(0, 10000, &dice, 0);
    return (((double)dice) / 100) < PHOTON_G(profiler_sampling_freq);
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
    next->timer_monotonic = clock_gettime_as_ns(CLOCK_MONOTONIC_RAW);
    next->timer_cpu = clock_gettime_as_ns(CLOCK_THREAD_CPUTIME_ID);

    // TODO: Copy endpoint name from previous or not?
    if (NULL != endpoint_name) {
        next->endpoint_name = estrdup(endpoint_name);
    } else {
        next->endpoint_name = estrdup(photon_get_default_endpoint_name());
    }

    if (NULL != prev) {
        // TODO: This is a matter of discussion, maybe should reset to 0
        next->stack_depth = prev->stack_depth;
        next->profiler_enable = prev->profiler_enable;
        next->app_name = estrdup(prev->app_name);
        next->app_version = estrdup(prev->app_version);
        next->app_env = estrdup(prev->app_env);
    } else {
        next->stack_depth = 0;
        next->profiler_enable = photon_should_profile();
        next->app_name = estrdup(PHOTON_G(app_name));
        next->app_version = estrdup(PHOTON_G(app_version));
        next->app_env = estrdup(PHOTON_G(app_env));
    }

    if (next->profiler_enable) {
        next->profiler_spans = emalloc(sizeof(zend_llist));
        zend_llist_init(next->profiler_spans, sizeof(profiler_span *), (llist_dtor_func_t) photon_profiler_span_dtor, 0);
    }

    // Push pointer to the transaction onto stack
    zend_ptr_stack_push(PHOTON_TXN_STACK, (void *)next);
}

static void photon_txn_dtor(transaction *txn)
{
    efree(txn->id);
    efree(txn->app_name);
    efree(txn->app_version);
    efree(txn->app_env);
    efree(txn->endpoint_name);
}

static void photon_profiler_span_dtor(profiler_span *span)
{
    efree(span->name);
    efree(span);
}

static void photon_txn_end()
{
    transaction *txn = zend_ptr_stack_pop(PHOTON_TXN_STACK);

    // TODO: If this is root transaction, we can add `SG(sapi_headers).http_response_code)` to report (non-CLI)
    // TODO: Also, there is `EG(exit_status)`. When exception is thrown, it's 255 (under any SAPI), and `exit(N)` works

    // TODO: Need to quote strings and escape double quotes
    // Write transaction info into log file
    fprintf(
        PHOTON_TXN_LOG,
        "%s,%s,%s,%s,%s,%s,%"PRIu64",%"PRIu64",%zu,%zu,%"PRIu64"\n",
        txn->app_name,
        txn->app_version,
        txn->app_env,
        sapi_module.name,
        txn->endpoint_name,
        txn->id,
        clock_gettime_as_ns(CLOCK_MONOTONIC_RAW) - txn->timer_monotonic,
        clock_gettime_as_ns(CLOCK_THREAD_CPUTIME_ID) - txn->timer_cpu,
        zend_memory_usage(0),
        zend_memory_usage(1),
        txn->timestamp
    );

    if (txn->profiler_enable) {
        // TODO: Create file using path from config
        // ID already is zero-terminated, so don't add +1 to length
        char *filename = emalloc(sizeof("/tmp/profile-") + sizeof(txn->id));
        strcpy(filename, "/tmp/profile-");
        strcat(filename, (const char *)&txn->id);
        // TODO: If opening failed, bailout
        FILE *fp = fopen(filename, "a");

        // TODO: Use `zend_llist_apply_with_del`
        zend_llist_element *element, *next;
        element = txn->profiler_spans->head;
        while (element) {
            next = element->next;
            // Watch out: double pointers
            profiler_span *span = *(profiler_span **)element->data;

            fprintf(
                fp,
                "%d,%s,%"PRIu64",%"PRIu64"\n",
                span->stack_depth,
                span->name,
                span->duration_monotonic,
                span->duration_cpu
            );

            element = next;
        }

        // Flush & close file
        fflush(fp);
        fclose(fp);
        efree(filename);

        // Destroy spans list
        // Destructor will be applied to all elements, see this linked list init
        zend_llist_destroy(txn->profiler_spans);
        efree(txn->profiler_spans);
    }

    photon_txn_dtor(txn);
    efree(txn);
}

PHP_FUNCTION(photon_get_txn_id) {
    if (PHOTON_NOT_ENABLED) {
        RETURN_NULL();
    }

    // TODO: Could null occur here?
    transaction *txn = photon_get_current_txn();
    RETURN_STRING(txn->id);
}

PHP_FUNCTION(photon_get_txn_app_name) {
    if (PHOTON_NOT_ENABLED) {
        RETURN_NULL();
    }

    // TODO: Could null occur here?
    transaction *txn = photon_get_current_txn();
    RETURN_STRING(txn->app_name);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_photon_set_txn_app_name, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(photon_set_txn_app_name) {
    zend_string *value = NULL;

    // TODO: This produces a warning and returns null. Would it be better to fail with error?
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR_EX(value, 1, 0)
    ZEND_PARSE_PARAMETERS_END();

    if (0 == ZSTR_LEN(value)) {
        php_error_docref(NULL, E_WARNING, "app name must be a non-empty string");
        RETURN_FALSE;
    }

    // TODO: Is everything correct here?
    transaction *txn = photon_get_current_txn();
    txn->app_name = estrdup(ZSTR_VAL(value));

    zend_string_release(value);
    RETURN_TRUE;
}

// TODO: Getters & setters: app name+version+env, endpoint
// TODO: Getters: id, mode
static const zend_function_entry photon_functions[] = {
    PHP_FE(photon_get_txn_id, NULL)
    PHP_FE(photon_get_txn_app_name, NULL)
    PHP_FE(photon_set_txn_app_name, arginfo_photon_set_txn_app_name)
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
