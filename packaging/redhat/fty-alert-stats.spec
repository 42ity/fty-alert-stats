#
#    fty-alert-stats - Agent for computing aggregate statistics on alerts
#
#    Copyright (C) 2014 - 2018 Eaton
#
#    This program is free software; you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation; either version 2 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License along
#    with this program; if not, write to the Free Software Foundation, Inc.,
#    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

# To build with draft APIs, use "--with drafts" in rpmbuild for local builds or add
#   Macros:
#   %_with_drafts 1
# at the BOTTOM of the OBS prjconf
%bcond_with drafts
%if %{with drafts}
%define DRAFTS yes
%else
%define DRAFTS no
%endif
%define SYSTEMD_UNIT_DIR %(pkg-config --variable=systemdsystemunitdir systemd)
Name:           fty-alert-stats
Version:        1.0.0
Release:        1
Summary:        agent for computing aggregate statistics on alerts
License:        GPL-2.0+
URL:            https://42ity.org
Source0:        %{name}-%{version}.tar.gz
Group:          System/Libraries
# Note: ghostscript is required by graphviz which is required by
#       asciidoc. On Fedora 24 the ghostscript dependencies cannot
#       be resolved automatically. Thus add working dependency here!
BuildRequires:  ghostscript
BuildRequires:  asciidoc
BuildRequires:  automake
BuildRequires:  autoconf
BuildRequires:  libtool
BuildRequires:  pkgconfig
BuildRequires:  systemd-devel
BuildRequires:  systemd
%{?systemd_requires}
BuildRequires:  xmlto
BuildRequires:  gcc-c++
BuildRequires:  libsodium-devel
BuildRequires:  zeromq-devel
BuildRequires:  czmq-devel
BuildRequires:  malamute-devel
BuildRequires:  cxxtools-devel
BuildRequires:  log4cplus-devel
BuildRequires:  tntdb-devel
BuildRequires:  tntnet-devel
BuildRequires:  openssl-devel
BuildRequires:  cyrus-sasl-devel
BuildRequires:  fty-common-devel
BuildRequires:  fty-proto-devel
BuildRoot:      %{_tmppath}/%{name}-%{version}-build

%description
fty-alert-stats agent for computing aggregate statistics on alerts.

%package -n libfty_alert_stats0
Group:          System/Libraries
Summary:        agent for computing aggregate statistics on alerts shared library

%description -n libfty_alert_stats0
This package contains shared library for fty-alert-stats: agent for computing aggregate statistics on alerts

%post -n libfty_alert_stats0 -p /sbin/ldconfig
%postun -n libfty_alert_stats0 -p /sbin/ldconfig

%files -n libfty_alert_stats0
%defattr(-,root,root)
%{_libdir}/libfty_alert_stats.so.*

%package devel
Summary:        agent for computing aggregate statistics on alerts
Group:          System/Libraries
Requires:       libfty_alert_stats0 = %{version}
Requires:       libsodium-devel
Requires:       zeromq-devel
Requires:       czmq-devel
Requires:       malamute-devel
Requires:       cxxtools-devel
Requires:       log4cplus-devel
Requires:       tntdb-devel
Requires:       tntnet-devel
Requires:       openssl-devel
Requires:       cyrus-sasl-devel
Requires:       fty-common-devel
Requires:       fty-proto-devel

%description devel
agent for computing aggregate statistics on alerts development tools
This package contains development files for fty-alert-stats: agent for computing aggregate statistics on alerts

%files devel
%defattr(-,root,root)
%{_includedir}/*
%{_libdir}/libfty_alert_stats.so
%{_libdir}/pkgconfig/libfty_alert_stats.pc
%{_mandir}/man3/*
%{_mandir}/man7/*

%prep

%setup -q

%build
sh autogen.sh
%{configure} --enable-drafts=%{DRAFTS} --with-systemd-units --with-tntnet=yes
make %{_smp_mflags}

%install
make install DESTDIR=%{buildroot} %{?_smp_mflags}

# remove static libraries
find %{buildroot} -name '*.a' | xargs rm -f
find %{buildroot} -name '*.la' | xargs rm -f

%files
%defattr(-,root,root)
%doc README.md
%{_bindir}/fty-alert-stats
%{_mandir}/man1/fty-alert-stats*
%config(noreplace) %{_sysconfdir}/fty-alert-stats/fty-alert-stats.cfg
%{SYSTEMD_UNIT_DIR}/fty-alert-stats.service
%dir %{_sysconfdir}/fty-alert-stats
%if 0%{?suse_version} > 1315
%post
%systemd_post fty-alert-stats.service
%preun
%systemd_preun fty-alert-stats.service
%postun
%systemd_postun_with_restart fty-alert-stats.service
%endif

%changelog
