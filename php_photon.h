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

typedef struct _transaction {
    // See https://stackoverflow.com/questions/51053568/generating-a-random-uuid-in-c
    char     id[37];
    char    *app_name;
    char    *app_version;
    char    *endpoint_name;
    uint64_t timestamp;
    uint64_t timer_monotonic;
    uint64_t timer_cpu;
} transaction;

typedef void (*callback)(zend_execute_data *);

typedef struct _interceptor {
    char *name;
    callback fn;
} interceptor;

ZEND_BEGIN_MODULE_GLOBALS(photon)
    zend_bool enable;
    char *transaction_log_path;

    FILE       *transaction_log;
    zend_stack *transaction_stack;
    HashTable  *interceptor_table;
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

ZEND_API static zend_always_inline void photon_execute_base(char internal, zend_execute_data *execute_data, zval *return_value);
ZEND_API static void photon_execute_internal(zend_execute_data *execute_data, zval *return_value);
ZEND_API static void photon_execute_ex (zend_execute_data *execute_data);

static zend_always_inline uint64_t clock_gettime_as_ns(clockid_t clk_id);
static zend_always_inline int extension_loaded(char *extension_name);

static void photon_txn_start(char *endpoint_name);
static void photon_txn_end();
static void photon_txn_dtor(transaction *txn);
static zend_always_inline transaction *photon_get_current_txn();
static char *photon_get_default_endpoint_name();

PHP_MINIT_FUNCTION(photon);
PHP_MSHUTDOWN_FUNCTION(photon);
PHP_MINFO_FUNCTION(photon);

PHP_RINIT_FUNCTION(photon);
PHP_RSHUTDOWN_FUNCTION(photon);

#endif /* PHP_PHOTON_H */
