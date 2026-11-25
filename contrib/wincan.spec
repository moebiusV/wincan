# wincan.spec -- RPM packaging for Fedora, RHEL and relatives (candled, and the Windows files).
# BEST EFFORT AND NOT YET TESTED on an RPM system; the Debian packaging in debian/ is the tested one.
Name:           wincan
Version:        1.1.0
Release:        1%{?dist}
Summary:        Candle server and Windows candle window
License:        GPL-3.0-or-later AND ISC
URL:            https://github.com/moebiusV/wincan
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.gz
BuildRequires:  gcc make sqlite-devel libcurl-devel openssl-devel systemd-rpm-macros
BuildRequires:  mingw64-gcc

%description
candled serves candles as JSON over TLS, to clients whose key it has been told by fingerprint; wincan.exe is the
small always-on-top candle chart window for Windows that shows them.

%package -n candled
Summary:        Candle server for wincan
Requires(pre):  shadow-utils
%{?systemd_requires}

%description -n candled
candled serves candles from a collector's files or a SQLite database as JSON over HTTP.  It listens on the loopback
address, speaks TLS, and answers only the clients whose key it has been authorized to accept (candled --authorize).

%package windows
Summary:        wincan, the candle window for Windows, with its installer
BuildArch:      noarch

%description windows
wincan.exe and a PowerShell installer for it, for use from a Linux system (WSL2, for example).

%prep
%autosetup

%build
%configure --docdir=%{_docdir}/candled --with-systemdsystemunitdir=%{_unitdir}
%make_build

%check
make check

%install
%make_install

%pre -n candled
getent group candled >/dev/null || groupadd -r candled
getent passwd candled >/dev/null || useradd -r -g candled -d %{_sharedstatedir}/candled -s /sbin/nologin -c "candled" candled
exit 0

%post -n candled
mkdir -p %{_sharedstatedir}/candled/candles /etc/candled
chown -R candled:candled %{_sharedstatedir}/candled
chown root:candled /etc/candled; chmod 750 /etc/candled
if [ ! -e /etc/candled/server.key ]; then
    %{_sbindir}/candled --gen-cert --certificate /etc/candled/server.crt --key /etc/candled/server.key >/dev/null
    chown root:candled /etc/candled/server.key /etc/candled/server.crt; chmod 640 /etc/candled/server.key
fi
[ -e /etc/candled/authorized-clients ] || { : > /etc/candled/authorized-clients; chown root:candled /etc/candled/authorized-clients; chmod 640 /etc/candled/authorized-clients; }
%systemd_post candled.service

%preun -n candled
%systemd_preun candled.service

%postun -n candled
%systemd_postun_with_restart candled.service

%files -n candled
%license COPYING
%doc %{_docdir}/candled
%config(noreplace) %{_sysconfdir}/candled.conf
%{_sbindir}/candled
%{_unitdir}/candled.service
%{_mandir}/man8/candled.8*
%{_mandir}/man5/candled.conf.5*
%{_mandir}/man7/candle-protocol.7*

%files windows
%license COPYING
%{_datadir}/wincan
%{_mandir}/man1/wincan.1*
%{_mandir}/man5/wincan.5*

%changelog
* Wed Nov 25 2026 David Walther <david@clearbrookdistillery.com> - 1.1.0-1
- wincan makes its own key and asks for the server at the first start; plain HTTP only for this computer.

* Tue Nov 24 2026 David Walther <david@clearbrookdistillery.com> - 1.0.0-1
- First release.
