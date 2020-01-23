PHP_ARG_ENABLE([photon],
  [whether to enable photon support],
  [AS_HELP_STRING([--enable-photon],
    [Enable photon support])],
  [no])

PHP_ARG_WITH([libuuid-dir],
  [],
  [AS_HELP_STRING([[--with-libuuid-dir=<DIR>]],
    [path to libuuid headers])],
  [no])

if test "$PHP_PHOTON" != "no"; then
  dnl Check for libuuid
  AC_MSG_CHECKING([for the location of libuuid])
  for dir in $PHP_LIBUUID_DIR /usr/local /usr; do
    if test -f "$dir/include/uuid/uuid.h"; then
      LIBUUID_DIR="$dir"
    fi
  done
  if test -z "$LIBUUID_DIR"; then
    AC_MSG_ERROR(not found)
  else
    AC_MSG_RESULT(found in $LIBUUID_DIR)
  fi
  PHP_ADD_INCLUDE($LIBUUID_DIR/include)
  PHP_CHECK_FUNC_LIB(uuid_generate_random, uuid)
  PHP_CHECK_FUNC_LIB(uuid_unparse_lower, uuid)

  AC_CHECK_HEADER([uuid/uuid.h], [], AC_MSG_ERROR('uuid/uuid.h' header not found))
  dnl Do not use `PHP_ADD_LIBRARY_WITH_PATH`: it's not independent on macOS. Just add location as a _possible_ one
  dnl so that it would be picked up in Linux.
  PHP_ADD_LIBPATH($LIBUUID_DIR/$PHP_LIBDIR)
  PHP_ADD_LIBRARY(uuid)
  PHP_SUBST(PHOTON_SHARED_LIBADD)

  AC_DEFINE(HAVE_PHOTON, 1, [ Have photon support ])

  PHP_NEW_EXTENSION(photon, photon.c, $ext_shared)
fi
