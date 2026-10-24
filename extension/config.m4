PHP_ARG_ENABLE(meminfo, whether to enable Memory Information support,
[ --enable-meminfo   Enable Meminfo support])

PHP_ARG_WITH(meminfo-version, for the version meminfo reports,
[  --with-meminfo-version=VER
                          Version reported by phpversion('meminfo') and phpinfo()
                          (default: the content of extension/VERSION)], no, no)

PHP_ARG_WITH(meminfo-copyright-year, for the meminfo copyright year,
[  --with-meminfo-copyright-year=YYYY
                          Last year of the copyright notice (default: the year of
                          SOURCE_DATE_EPOCH when set, else the current year)], no, no)

if test "$PHP_MEMINFO" = "yes"; then
  AC_DEFINE(HAVE_MEMINFO, 1, [Whether you have Memory Info])

  dnl The version becomes a C string literal: no quotes, backslashes or spaces.
  meminfo_version="$PHP_MEMINFO_VERSION"
  if test "$meminfo_version" = "no" || test -z "$meminfo_version"; then
    meminfo_version=`cat "PHP_EXT_SRCDIR(meminfo)/VERSION" 2>/dev/null | tr -d ' \r\n'`
  fi
  if test -z "$meminfo_version" || test "$meminfo_version" = "yes" \
     || echo "$meminfo_version" | grep '[[^0-9A-Za-z.+~-]]' >/dev/null; then
    AC_MSG_ERROR([invalid meminfo version '$meminfo_version': use only letters, digits and . + ~ -])
  fi

  meminfo_year="$PHP_MEMINFO_COPYRIGHT_YEAR"
  if test "$meminfo_year" = "no" || test -z "$meminfo_year"; then
    if test -n "$SOURCE_DATE_EPOCH"; then
      dnl GNU and BusyBox date take -d @N, BSD date takes -r N.
      meminfo_year=`date -u -d "@$SOURCE_DATE_EPOCH" +%Y 2>/dev/null || date -u -r "$SOURCE_DATE_EPOCH" +%Y 2>/dev/null`
    else
      meminfo_year=`date -u +%Y`
    fi
  fi
  if echo "$meminfo_year" | grep '^[[0-9]][[0-9]][[0-9]][[0-9]]$' >/dev/null; then :; else
    AC_MSG_ERROR([invalid meminfo copyright year '$meminfo_year': expected four digits])
  fi

  AC_MSG_CHECKING([meminfo version and copyright year])
  AC_MSG_RESULT([$meminfo_version, $meminfo_year])
  AC_DEFINE_UNQUOTED(MEMINFO_VERSION, ["$meminfo_version"], [Version reported by the extension])
  AC_DEFINE_UNQUOTED(MEMINFO_COPYRIGHT_YEAR, ["$meminfo_year"], [Last year of the copyright notice])

  dnl sigqueue() carries an integer with the signal, which is how
  dnl meminfo_dump_process() tells the target which file name to use and
  dnl recognises its own dump. Without it (macOS) the trigger falls back to
  dnl kill() and matches the dump by pid and time.
  AC_CHECK_FUNCS(sigqueue)

  dnl Every source is listed unconditionally: a module whose platform or PHP
  dnl version guard is off compiles to an empty object. Keep in step with
  dnl CMakeLists.txt.
  PHP_NEW_EXTENSION(meminfo, meminfo.c writer.c ptr_set.c traverse.c items.c fibers.c signal.c procinfo.c trigger.c generations.c sites.c, $ext_shared)

  dnl Errors are reported by throwing SPL exceptions, so SPL has to be
  dnl initialised before this extension is.
  PHP_ADD_EXTENSION_DEP(meminfo, spl)
fi
