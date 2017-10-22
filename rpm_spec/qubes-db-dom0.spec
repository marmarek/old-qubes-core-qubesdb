#
# This is the SPEC file for creating binary and source RPMs for the Qubes DB.
#
#
# The Qubes OS Project, http://www.qubes-os.org
#
# Copyright (C) 2013  Marek Marczykowski <marmarek@invisiblethingslab.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#
#

%{!?version: %define version %(cat version)}

# Package contains /usr/lib, but not binary files, which confuses find-debuginfo.sh script.
%global debug_package %{nil}

Name:		qubes-db-dom0
Version:	%{version}
Release:	1%{?dist}
Summary:	QubesDB VM service

Group:		Qubes
License:	GPL
URL:		http://www.qubes-os.org/

Requires:	qubes-db
Requires(post): /bin/systemctl

%define _builddir %(pwd)

%description
QubesDB daemon service.

%prep
# we operate on the current directory, so no need to unpack anything
# symlink is to generate useful debuginfo packages
rm -f %{name}-%{version}
ln -sf . %{name}-%{version}
%setup -T -D

%build

%install
install -d %{buildroot}%{_unitdir}
install -p -m 644 daemon/qubes-db-dom0.service %{buildroot}%{_unitdir}/

%post
systemctl enable qubes-db-dom0.service > /dev/null

%preun
if [ $1 -eq 0 ]; then
    systemctl disable qubes-db-dom0.service > /dev/null
fi

%files
%doc
%{_unitdir}/qubes-db-dom0.service

%changelog

