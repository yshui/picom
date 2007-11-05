bin_PROGRAMS = xcompmgr
man_MANS = xcompmgr.1

xcompmgr_LDADD = @XCOMPMGR_LIBS@ -lm

INCLUDES = @XCOMPMGR_CFLAGS@

EXTRA_DIST = \
	autogen.sh \
	ChangeLog \
	${man_MANS}

MAINTAINERCLEANFILES=ChangeLog

.PHONY: ChangeLog

ChangeLog:
	(GIT_DIR=$(top_srcdir)/.git git-log > .changelog.tmp && mv .changelog.tmp ChangeLog; rm -f .changelog.tmp) || (touch ChangeLog; echo 'git directory not found: installing possibly empty changelog.' >&2)

dist-hook: ChangeLog

