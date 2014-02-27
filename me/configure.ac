dnl Process this file with autoconf to produce a configure script.

AC_PREREQ(2.57)
AC_INIT([me], [0.0.1], [nichamon@ogc.us])
AC_CONFIG_MACRO_DIR([m4])
dnl AC_CONFIG_SRCDIR([src/me.h])
AC_CONFIG_AUX_DIR(config)
AM_CONFIG_HEADER(config.h)
AM_INIT_AUTOMAKE([-Wno-portability])
AM_PROG_LIBTOOL

AC_ARG_ENABLE(debug, [  --enable-debug    Enable debug mode],
	[
		if test x$enableval = xyes ; then
			CFLAGS="-g -O0 -DDEBUG -Werror"
			echo "debug is enabled"
		fi
	]
)

OPTION_DEFAULT_ENABLE([ovis-lib], [ENABLE_OVIS_LIB])
OPTION_WITH([ovis-lib], [OVIS_LIB])

OPTION_DEFAULT_ENABLE([ocm], [ENABLE_OCM])
OPTION_WITH([ocm], [OCM])

OPTION_DEFAULT_ENABLE([core], [ENABLE_CORE])
OPTION_DEFAULT_ENABLE([me], [ENABLE_ME])

dnl Options for models
OPTION_DEFAULT_ENABLE([models], [ENABLE_MODELS])
OPTION_DEFAULT_ENABLE([model_example], [ENABLE_MODEL_EXAMPLE])
OPTION_DEFAULT_ENABLE([bkde], [ENABLE_BKDE])
OPTION_DEFAULT_ENABLE([baler_rules], [ENABLE_BALER_RULES])
OPTION_DEFAULT_ENABLE([high_outlier], [ENABLE_HIGH_OUTLIER])
OPTION_DEFAULT_ENABLE([high_exact], [ENABLE_HIGH_EXACT])
OPTION_DEFAULT_ENABLE([low_exact], [ENABLE_LOW_EXACT])
OPTION_DEFAULT_ENABLE([low_outlier], [ENABLE_LOW_OUTLIER])
OPTION_DEFAULT_ENABLE([above_mean], [ENABLE_ABOVE_MEAN])
OPTION_DEFAULT_ENABLE([below_mean], [ENABLE_BELOW_MEAN])

dnl Options for stores
OPTION_DEFAULT_ENABLE([stores], [ENABLE_STORES])
OPTION_DEFAULT_ENABLE([store_example], [ENABLE_STORE_EXAMPLE])
OPTION_DEFAULT_DISABLE([store_sos], [ENABLE_STORE_SOS])
OPTION_DEFAULT_ENABLE([sos], [ENABLE_SOS])
OPTION_WITH([sos], [SOS])

dnl Options for consumers
OPTION_DEFAULT_ENABLE([consumers], [ENABLE_CONSUMERS])
OPTION_DEFAULT_ENABLE([kmd], [ENABLE_KMD])

dnl Options for tests
OPTION_DEFAULT_DISABLE([test], [ENABLE_TEST])

dnl Checks for programs
AC_PROG_CC
AC_PROG_CXX
AC_CHECK_SIZEOF(long)
dnl AM_PROG_CC_C_O

dnl Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST

dnl Check for pthread support
AC_CHECK_LIB(pthread, pthread_mutex_init, [],
    AC_MSG_ERROR([pthread library not found.  me-utils requires libpthread.]))
LIBS=""

dnl Checks for libraries
AC_CHECK_LIB(zap, zap_new, [],
	AC_MSG_ERROR([zap library not found. Baler requires libzap. libzap is
		      one of the libraries in lib-ovis (please see OVIS/lib)]),
	$OVIS_LIB_LIBDIR_FLAG)
LIBS=""

if test -z "$ENABLE_OCM_TRUE"
then
AC_CHECK_LIB(ocm, ocm_create, [],
	     AC_MSG_ERROR([ocm library not found. Please see OVIS/ocm.]),
	     $OCM_LIBDIR_FLAG
	     )
LIBS=""
fi

AC_CHECK_LIB(ovis_util, av_value, [],
	     AC_MSG_ERROR([ovis_util library not found.
			   Please install ovis_util from OVIS/lib. ]),
	     $OVIS_LIB_LIBDIR_FLAG )
dnl Reset LIBS variable.
LIBS=""

AC_CHECK_LIB(coll, idx_create, [],
	     AC_MSG_ERROR([coll library not found.
			   Please install coll from OVIS/lib. ]),
	     $OVIS_LIB_LIBDIR_FLAG )
dnl Reset LIBS variable.
LIBS=""

AC_CACHE_CHECK(whether ld accepts --version-script, ac_cv_version_script,
    if test -n "`$LD --help < /dev/null 2>/dev/null | grep version-script`"; then
	ac_cv_version_script=yes
    else
	ac_cv_version_script=no
    fi)

dnl Reset LIBS variable
LIBS=""

AC_SUBST(ac_configure_args)
AC_CONFIG_FILES([Makefile src/Makefile
		src/core/Makefile src/me/Makefile
		src/models/Makefile
		src/stores/Makefile
		src/consumers/Makefile
		src/test/Makefile])
AC_OUTPUT
