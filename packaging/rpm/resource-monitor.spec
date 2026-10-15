# packaging/rpm/resource-monitor.spec
%global debug_package %{nil}

Name:           resource-monitor
Version:        1.0.1
Release:        1%{?dist}
Summary:        System resource snapshots with reloadable JSON thresholds
License:        Proprietary
Source0:        %{name}-%{version}.tar.gz
Source1:        json.tar.xz
BuildRequires:  gcc-toolset-15-gcc-c++
BuildRequires:  cmake
BuildRequires:  make
BuildRequires:  python3
Requires(pre):  shadow-utils
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd

%description
C++23 system resource daemon for Oracle Linux 8 and 9. Collects CPU, memory
and local mounted filesystem metrics every minute. Writes atomic timestamped
JSON snapshots only when at least one configured threshold is reached.

%prep
%setup -q
mkdir json-dependency
echo '42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa  %{SOURCE1}' | sha256sum -c -
tar -xf %{SOURCE1} -C json-dependency --strip-components=1

%build
. /opt/rh/gcc-toolset-15/enable
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=ON -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$PWD/json-dependency"
cmake --build build --parallel %{?_smp_build_ncpus}

%check
. /opt/rh/gcc-toolset-15/enable
ctest --test-dir build --output-on-failure

%install
DESTDIR=%{buildroot} cmake --install build
install -d -m 0750 %{buildroot}/Monitor

%pre
getent group resource-monitor >/dev/null || groupadd --system resource-monitor
getent passwd resource-monitor >/dev/null || useradd --system --gid resource-monitor --home-dir /nonexistent --shell /sbin/nologin resource-monitor

%post
systemctl daemon-reload >/dev/null 2>&1 || :

%preun
if [ "$1" -eq 0 ]; then
    systemctl --no-reload disable --now resource-monitor.service >/dev/null 2>&1 || :
fi

%postun
systemctl daemon-reload >/dev/null 2>&1 || :
if [ "$1" -ge 1 ]; then
    systemctl try-restart resource-monitor.service >/dev/null 2>&1 || :
fi

%files
%attr(0755,root,root) /usr/sbin/resource-monitor
/usr/lib/systemd/system/resource-monitor.service
%dir /etc/resource-monitor
%config(noreplace) /etc/resource-monitor/thresholds.json
%dir %attr(0750,resource-monitor,resource-monitor) /Monitor
/usr/share/resource-monitor
%dir /usr/share/doc/resource-monitor
%doc /usr/share/doc/resource-monitor/README.md
%license /usr/share/doc/resource-monitor/THIRD_PARTY_NOTICES.txt
