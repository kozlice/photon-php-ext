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
#define PHP_PHOTON_H

// TODO: Only keep what we need
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <inttypes.h>
#include <uuid/uuid.h>

extern zend_module_entry photon_module_entry;
#define phpext_photon_ptr &photon_module_entry

#define PHP_PHOTON_NAME "photon"
#define PHP_PHOTON_VERSION "0.1.0"

#if defined(ZTS) && defined(COMPILE_DL_PHOTON)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

typedef struct _profiler_span {
    char    *name;
    int      stack_depth;
    uint64_t duration_monotonic;
    uint64_t duration_cpu;
} profiler_span;

typedef struct _transaction {
    // See https://stackoverflow.com/questions/51053568/generating-a-random-uuid-in-c
    char     id[37];
    char    *app_name;
    char    *app_version;
    char    *app_env;
    char    *endpoint_name;
    int      stack_depth;
    uint64_t timestamp;
    uint64_t timer_monotonic;
    uint64_t timer_cpu;

    zend_bool   profiler_enable;
    zend_llist *profiler_spans;
} transaction;

typedef void (*interceptor_handler)(zend_execute_data *);

typedef struct _interceptor {
    char *name;
    interceptor_handler fn;
} interceptor;

ZEND_BEGIN_MODULE_GLOBALS(photon)
    // Configuration: per module, auto-allocation
    zend_bool enable;
    char *transaction_log_path;
    char *profiler_output_dir;
    zend_bool profiler_enable;
    zend_bool profiler_enable_cli;
    double profiler_sampling_freq;
    char *profiler_trigger_http_header;
    char *profiler_trigger_query_param;
    char *profiler_trigger_cookie_name;
    char *app_name;
    char *app_version;
    char *app_env;

    // Per module, using `pemalloc`
    FILE      *transaction_log;
    HashTable *interceptor_table;

    // Per request, using `emalloc`
    zend_ptr_stack *transaction_stack;
ZEND_END_MODULE_GLOBALS(photon)

ZEND_EXTERN_MODULE_GLOBALS(photon)

// Define globals accessor
// Note: spaces are required
#ifdef ZTS
# define PHOTON_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(photon, v)
#else
# define PHOTON_G(v) (photon_globals.v)
#endif

#define PHOTON_NOT_ENABLED  0 == PHOTON_G(enable)
#define PHOTON_TXN_LOG      PHOTON_G(transaction_log)
#define PHOTON_TXN_STACK    PHOTON_G(transaction_stack)
#define PHOTON_INTERCEPTORS PHOTON_G(interceptor_table)
#define PHOTON_ERROR(format, ...) fprintf(stderr, format, __VA_ARGS__)

#define PHOTON_ITC_SEPARATOR "::"

ZEND_API static zend_always_inline void photon_execute_base(char internal, zend_execute_data *execute_data, zval *return_value);
ZEND_API static void photon_execute_internal(zend_execute_data *execute_data, zval *return_value);
ZEND_API static void photon_execute_ex (zend_execute_data *execute_data);

static zend_always_inline uint64_t clock_gettime_as_ns(clockid_t clk_id);
static zend_always_inline int extension_loaded(char *extension_name);

static zend_bool photon_should_profile();
static void photon_txn_start(char *endpoint_name);
static void photon_txn_end();
static void photon_txn_dtor(transaction *txn);
static zend_always_inline transaction *photon_get_current_txn();
static void photon_interceptor_add(char *name, interceptor_handler fn);
static void photon_interceptor_dtor(zval *entry);
static void photon_profiler_span_dtor(profiler_span *span);
static char *photon_get_default_endpoint_name();

PHP_MINIT_FUNCTION(photon);
PHP_MSHUTDOWN_FUNCTION(photon);
PHP_MINFO_FUNCTION(photon);

PHP_RINIT_FUNCTION(photon);
PHP_RSHUTDOWN_FUNCTION(photon);

PHP_FUNCTION(photon_get_txn_id);
PHP_FUNCTION(photon_get_txn_app_name);
PHP_FUNCTION(photon_set_txn_app_name);

#endif /* PHP_PHOTON_H */
