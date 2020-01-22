PHP_ARG_ENABLE([photon],
  [whether to enable photon support],
  [AS_HELP_STRING([--enable-photon],
    [Enable photon support])],
  [no])

if test "$PHP_PHOTON" != "no"; then
  # Check for libuuid
  AC_MSG_CHECKING([for the location of libuuid])
  for dir in /usr/local /usr; do
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
  PHP_SUBST(PHOTON_SHARED_LIBADD)
  PHP_ADD_LIBRARY_WITH_PATH(uuid, $LIBUUID_DIR/$PHP_LIBDIR, PHOTON_SHARED_LIBADD)

  dnl In case of no dependencies
  AC_DEFINE(HAVE_PHOTON, 1, [ Have photon support ])

  PHP_NEW_EXTENSION(photon, photon.c, $ext_shared)
fi
