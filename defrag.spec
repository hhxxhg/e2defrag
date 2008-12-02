Description: Linux filesystem defragmenter
Name: defrag
Version: 0.73pjm1
Release: 1
Copyright: GPL
Group: Utilities/System
Source: sunsite.unc.edu:/pub/Linux/system/filesystems/defrag-0.73pjm1.tar.gz

%prep
%setup
FIXME: Someone familiar with .spec files should update this for the
configure-related changes.  E.g. need to run configure (passing LDFLAGS
and CFLAGS in the environment -- see debian/rules).
The file list also needs to be updated.

%build
FIXME: Pass flags in environment of configure script, not on make commandline.
make OPTI="$RPM_OPT_FLAGS" LDFLAGS=-s

%install
make install

%files
%doc BUGS ChangeLog INSTALL NEWS README
FIXME: update this list.  E.g. default to /usr/sbin (except perhaps for e2defrag.static),
and there are extra manpages (symlinks).
At time of writing, `defrag' isn't written.  It used to be the minix version (now mdefrag);
when written, `defrag' will look for magic numbers and exec the right version of defrag.
/sbin/defrag
/sbin/e2defrag
/sbin/e2defrag.static
/sbin/edefrag
/sbin/mdefrag
/sbin/xdefrag
/sbin/e2dump
/sbin/xdump
/sbin/frag
/usr/man/man8/defrag.8
/usr/man/man8/frag.8
