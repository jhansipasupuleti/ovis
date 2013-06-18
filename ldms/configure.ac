dnl Process this file with autoconf to produce a configure script.

AC_PREREQ(2.57)
AC_INIT(ldms-utils, 2.1.0, tom@ogc.us)
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([src/ldms.h])
AC_CONFIG_AUX_DIR(config)
AM_CONFIG_HEADER(config.h)
AM_INIT_AUTOMAKE(ldms-utils, 2.1.0)
AM_PROG_LIBTOOL

AC_ARG_ENABLE(sos, [  --disable-sos     Disable the SOS plugin],
        [       if test x$enableval = xno ; then
                        disable_sos=yes
			echo SOS plugin is disabled
                fi
        ])

AC_ARG_ENABLE(papi, [  --disable-papi     Disable the PAPI plugin],
        [       if test x$enableval = xno ; then
                        disable_papi=yes
			echo PAPI plugin is disabled
                fi
        ])

AC_ARG_ENABLE(ugni, [  --disable-ugni     Disable the uGNI transport],
        [       if test x$enableval = xno ; then
                        disable_ugni=yes
			echo uGNI transport is disabled
                fi
        ])

AC_ARG_ENABLE(rdma, [  --disable-rdma     Disable the RDMA transport],
        [       if test x$enableval = xno ; then
                        disable_rdma=yes
			echo RDMA transport is disabled
                fi
        ])

AC_ARG_ENABLE(sock, [  --disable-sock     Disable the sockets transport],
        [       if test x$enableval = xno ; then
                        disable_sock=yes
			echo Socket transport is disabled
                fi
        ])

AC_ARG_ENABLE(mmap, [  --disable-mmap     Disable peristent local data sets (i.e. mmap).],
        [       if test x$enableval = xno ; then
                        disable_mmap=yes
			echo The mmap API is not supported on this system.
                fi
        ])

AC_ARG_ENABLE(perf, [  --disable-perf     Disable perf events.],
        [       if test x$enableval = xno ; then
                        disable_perf=yes
                        echo The perf events sampler has been disabled.
                fi
        ])

AC_ARG_ENABLE(mysql, [  --disable-mysql     Disable mysql inserts.],
        [       if test x$enableval = xno ; then
                        disable_mysql=yes
                        echo The mysqlinsert sampler has been disabled.
                fi
        ])

AC_ARG_ENABLE(sensors, [  --disable-sensors     Disable sensors.],
        [       if test x$enableval = xno ; then
                        disable_sensors=yes
                        echo The procsensors sampler has been disabled.
                fi
        ])

AC_ARG_ENABLE(glib, [  --disable-glib     Disable plugins/utilities using glib.],
        [       if test x$enableval = xno ; then
                        disable_glib=yes
                        echo The sedc sampler and hwloc utilities have been disabled.
                fi
        ])

AC_ARG_WITH(
	sos,
	AS_HELP_STRING(
		[--with-sos@<:@=path@:>@],
		[Specify sos path @<:@default=/usr/local@:>@]
	),
	[WITH_SOS=$withval],
	[WITH_SOS=/usr/local]
)

SOS_LIBDIR="$WITH_SOS/lib"
SOS_INCDIR="$WITH_SOS/include"

AC_ARG_ENABLE(yaml, [  --disable-yaml     Disable yaml support.],
        [       if test x$enableval = xno ; then
                        disable_yaml=yes
			echo yaml configuration file support has been disabled.
                fi
        ])

dnl Checks for programs
AC_PROG_CC
AC_PROG_CXX
AC_CHECK_SIZEOF(long)

dnl Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST

dnl Check for libevent
AC_CHECK_LIB(event, event_base_new, [],
    AC_MSG_ERROR([event_base_new() not found.  ldms-utils requires libevent.]))

dnl Check for pthread support
AC_CHECK_LIB(pthread, pthread_mutex_init, [],
    AC_MSG_ERROR([pthread library not found.  ldms-utils requires libpthread.]))

dnl Checks for libraries
if test "$disable_rdma" != "yes"
then
AC_CHECK_LIB(ibverbs, ibv_get_device_list, [],
    AC_MSG_ERROR([ibv_get_device_list() not found.  ldms-utils requires libibverbs.]))

dnl Checks for header files.
AC_CHECK_HEADER(infiniband/driver.h, [],
    AC_MSG_ERROR([<infiniband/driver.h> not found.  Is libibverbs installed?]))
AC_HEADER_STDC

dnl Checks for library functions
AC_CHECK_FUNCS(ibv_read_sysfs_file)

dnl Check for glib
PKG_CHECK_MODULES([GLIB20], [glib-2.0 >= 2.0.0])

dnl Now check if for libibverbs device library extension
dummy=if$$
cat <<IBV_VERSION > $dummy.c
#include <infiniband/driver.h>
IBV_DEVICE_LIBRARY_EXTENSION
IBV_VERSION
IBV_DEVICE_LIBRARY_EXTENSION=`$CC $CPPFLAGS -E $dummy.c 2> /dev/null | tail -1`
rm -f $dummy.c
if test $IBV_DEVICE_LIBRARY_EXTENSION = IBV_DEVICE_LIBRARY_EXTENSION; then
    AC_MSG_ERROR([IBV_DEVICE_LIBRARY_EXTENSION not defined.  Is libibverbs new enough?])
fi
AC_SUBST(IBV_DEVICE_LIBRARY_EXTENSION)
fi

if test "$disable_sos" != "yes"
then

AC_CHECK_LIB( sos, sos_open,
	[
		AC_CHECK_HEADERS($SOS_INCDIR/sos/sos.h,
		[
			AC_SUBST([SOS_LIBDIR], [$SOS_LIBDIR] )
			AC_SUBST([SOS_INCDIR], [$SOS_INCDIR])
		],
		[
			AC_MSG_FAILURE([sos/sos.h not found])
		],
		)
	],
	[
		AC_MSG_FAILURE(libsos not found)
	],
	-lpthread -lods -loidx -lidx -L$SOS_LIBDIR
)

fi
if test "$disable_yaml" != "yes"
then
AC_CHECK_LIB(yaml, yaml_parser_load, [],
	AC_MSG_ERROR([libyaml not found. please add --disable-yaml to disable yaml support.]))
fi

AC_CACHE_CHECK(whether ld accepts --version-script, ac_cv_version_script,
    if test -n "`$LD --help < /dev/null 2>/dev/null | grep version-script`"; then
        ac_cv_version_script=yes
    else
        ac_cv_version_script=no
    fi)

AM_CONDITIONAL([ENABLE_SOS], [test "$disable_sos" != "yes"])
AM_CONDITIONAL([ENABLE_PAPI], [test "$disable_papi" != "yes"])
AM_CONDITIONAL([ENABLE_UGNI], [test "$disable_ugni" != "yes"])
AM_CONDITIONAL([ENABLE_RDMA], [test "$disable_rdma" != "yes"])
AM_CONDITIONAL([ENABLE_SOCK], [test "$disable_sock" != "yes"])
AM_CONDITIONAL([ENABLE_MMAP], [test "$disable_mmap" != "yes"])
AM_CONDITIONAL([ENABLE_PERF], [test "$disable_perf" != "yes"])
AM_CONDITIONAL([ENABLE_MYSQL], [test "$disable_mysql" != "yes"])
AM_CONDITIONAL([ENABLE_SENSORS], [test "$disable_sensors" != "yes"])
AM_CONDITIONAL([ENABLE_GLIB], [test "$disable_glib" != "yes"])
AM_CONDITIONAL([ENABLE_YAML], [test "$disable_yaml" != "yes"])
AM_CONDITIONAL([HAVE_LD_VERSION_SCRIPT], [test "$ac_cv_version_script" = "yes"])
AC_CONFIG_FILES([Makefile ldms-utils.spec])
AC_OUTPUT
