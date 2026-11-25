dnl configure.ac -- wincan: candled, the candle server for Linux, and wincan.exe, the candle window for Windows.
dnl Process with `./bootstrap` (autoreconf); a release tarball already has the configure script.

AC_PREREQ([2.69])
AC_INIT([wincan], [1.1.2], [https://github.com/moebiusV/wincan/issues], [wincan], [https://github.com/moebiusV/wincan])
AC_CONFIG_SRCDIR([src/candled.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([1.13 gnu -Wall -Wno-portability subdir-objects tar-pax])
AM_SILENT_RULES([yes])

AC_USE_SYSTEM_EXTENSIONS
AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB
AC_PROG_INSTALL
AC_PROG_MKDIR_P

dnl -- the server, candled: sqlite3, libcurl, OpenSSL, threads, libm
AC_SEARCH_LIBS([sqrt], [m], [], [AC_MSG_ERROR([the math library is needed])])
AC_SEARCH_LIBS([pthread_create], [pthread], [], [AC_MSG_ERROR([POSIX threads are needed])])
AC_CHECK_HEADERS([sqlite3.h curl/curl.h openssl/ssl.h], [], [AC_MSG_ERROR([a header is missing: install the development files of sqlite3, libcurl and OpenSSL (libsqlite3-dev libcurl4-openssl-dev libssl-dev on Debian and Ubuntu; sqlite-devel libcurl-devel openssl-devel on Fedora; sqlite-dev curl-dev openssl-dev on Alpine)])])
AC_SEARCH_LIBS([sqlite3_open_v2], [sqlite3], [], [AC_MSG_ERROR([libsqlite3 is needed])])
AC_SEARCH_LIBS([curl_easy_init], [curl], [], [AC_MSG_ERROR([libcurl is needed])])
AC_SEARCH_LIBS([EVP_sha256], [crypto], [], [AC_MSG_ERROR([libcrypto (OpenSSL) is needed])])
AC_SEARCH_LIBS([SSL_CTX_new], [ssl], [], [AC_MSG_ERROR([libssl (OpenSSL) is needed])], [-lcrypto])
AC_CHECK_FUNCS([daemon], [], [AC_MSG_ERROR([daemon(3) is needed])])

dnl -- the window, wincan.exe: built with MinGW-w64, if there is one; it is a Windows program and has no part in the Linux build
AC_ARG_VAR([WINCC], [MinGW-w64 C compiler that builds wincan.exe])
AC_ARG_ENABLE([windows], [AS_HELP_STRING([--disable-windows], [do not build wincan.exe, even if MinGW-w64 is there])], [], [enable_windows=yes])
AS_IF([test "x$enable_windows" != xno],
      [AC_CHECK_PROGS([WINCC], [x86_64-w64-mingw32-gcc x86_64-w64-mingw32-gcc-win32 x86_64-w64-mingw32-gcc-posix])])
AM_CONDITIONAL([BUILD_WINCAN], [test -n "$WINCC"])
AS_IF([test -z "$WINCC" && test "x$enable_windows" != xno],
      [AC_MSG_NOTICE([no MinGW-w64 compiler: wincan.exe will not be built (the release has a built one); set WINCC to build it])])

dnl -- where the service files go
AC_ARG_WITH([systemdsystemunitdir],
            [AS_HELP_STRING([--with-systemdsystemunitdir=DIR], [install candled.service in DIR (for example /usr/lib/systemd/system); otherwise it is only an example])],
            [], [with_systemdsystemunitdir=no])
AM_CONDITIONAL([HAVE_SYSTEMD], [test "x$with_systemdsystemunitdir" != xno])
AC_SUBST([systemdsystemunitdir], [$with_systemdsystemunitdir])
AC_ARG_VAR([CANDLED_USER], [the user that candled runs as when installed as a service (default candled)])
AS_IF([test -z "$CANDLED_USER"], [CANDLED_USER=candled])
AC_SUBST([CANDLED_USER])

AC_CONFIG_FILES([Makefile contrib/candled.service contrib/candled.openrc])
AC_OUTPUT
