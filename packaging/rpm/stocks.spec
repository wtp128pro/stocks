%global app_id io.github.wtp128pro.Stocks

Name:           stocks
Version:        0.2.2
Release:        1%{?dist}
Summary:        GTK4 desktop app to follow shares, ETFs, mutual funds and indices

# The program and its icons are GPL-3.0-or-later; the AppStream metainfo file is CC0-1.0.
License:        GPL-3.0-or-later AND CC0-1.0
URL:            https://github.com/wtp128pro/stocks
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  pkgconfig(gtk4) >= 4.14
BuildRequires:  pkgconfig(libsoup-3.0)
BuildRequires:  pkgconfig(json-glib-1.0) >= 1.10.0
BuildRequires:  pkgconfig(glib-2.0) >= 2.68
BuildRequires:  pkgconfig(gio-2.0) >= 2.68
# glib-compile-resources (compiles the catalog and the CSS into the binary)
BuildRequires:  glib2-devel
# %%check
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib
BuildRequires:  appstream
BuildRequires:  xorg-x11-server-Xvfb

Requires:       hicolor-icon-theme

%description
Stocks is a small GTK4 desktop application. It lists the instruments you
follow with price, change and a one-month trend line, and shows a price chart
for the selected one (1D, 1M, 3M, 6M and 1Y). It includes a built-in catalog
of Fidelity ETFs and mutual funds and the main US market indices, and keeps a
local cache so it still shows data when offline.

Quotes come from unofficial, keyless web endpoints: NASDAQ for stocks, ETFs
and mutual funds, and Yahoo Finance for the four market indices. These are not
documented or supported public APIs; they are meant for personal, low-volume
use only and may change, throttle requests or stop working at any time.
Quotes are not guaranteed to be real-time.

Stocks does not trade, log in to a broker or sync a Fidelity account. It is
not affiliated with, endorsed by or sponsored by Fidelity, NASDAQ, Yahoo or
Apple; all names belong to their owners.

%prep
%autosetup

%build
%meson
%meson_build

%install
%meson_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{app_id}.desktop
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/%{app_id}.metainfo.xml
# The GUI tests need an X display; none of the tests uses the network.
# GTK_IM_MODULE=simple keeps GTK from connecting to an input-method daemon.
GTK_IM_MODULE=simple GTK_A11Y=none xvfb-run -a %{shrink:%meson_test}

%files
%license COPYING
%doc README.md
%{_bindir}/stocks
%{_datadir}/applications/%{app_id}.desktop
%{_metainfodir}/%{app_id}.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/%{app_id}.svg
%{_datadir}/icons/hicolor/symbolic/apps/%{app_id}-symbolic.svg

%changelog
* Wed Oct 14 2026 wtp128pro - 0.2.2-1
- Watchlist rows show the chart range chosen for each stock as a pill
  label instead of the one-month sparkline

* Tue Oct 13 2026 wtp128pro - 0.2.1-1
- Market indices: measure the change against the previous session's close
  from Yahoo's daily bars; Yahoo's previousClose field was stale

* Sat Oct 10 2026 wtp128pro - 0.2.0-1
- Remember the chart range and the percent/points choice of each stock
- Click the change label of a list row to switch between percent and points

* Fri Oct 09 2026 wtp128pro - 0.1.0-1
- Initial package
