# packaging/rpm/resource-monitor.spec
%global debug_package %{nil}

Name:           resource-monitor
Version:        1.7.0
Release:        1%{?dist}
Summary:        System resource snapshots with JSON settings
License:        Proprietary
Source0:        %{name}-%{version}.tar.gz
Source1:        json.tar.xz
BuildRequires:  gcc-toolset-15-gcc-c++
BuildRequires:  cmake
BuildRequires:  make
BuildRequires:  python3
BuildRequires:  pkgconf-pkg-config
BuildRequires:  mariadb-connector-c-devel
Requires(pre):  shadow-utils
Requires(pre):  coreutils
Requires(post): coreutils
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd

%description
C++23 system resource daemon for Oracle Linux 8 and 9. Collects CPU, memory
and local mounted filesystem metrics and role-specific Java DAPP process
presence at a configurable interval. Writes an
atomic timestamped JSON snapshot every collection cycle. Collection interval,
output directory, status thresholds, role-specific DAPP lists and optional MySQL connections reload
without restarting the daemon. Database credentials are stored separately.

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
install -d -m 0750 %{buildroot}/etc/resource-monitor/secrets

%pre
settings_file=/etc/resource-monitor/settings.json
legacy_file=/etc/resource-monitor/thresholds.json
legacy_backup=/etc/resource-monitor/thresholds.json.legacy
migration_file=/etc/resource-monitor/.settings.json.migrate
if [ -e "$migration_file" ] || [ -L "$migration_file" ]; then
    echo 'Settings migration file already exists: /etc/resource-monitor/.settings.json.migrate. Preserve and inspect it before retrying the installation.' >&2
    exit 1
fi
getent group resource-monitor >/dev/null || groupadd --system resource-monitor
getent passwd resource-monitor >/dev/null || useradd --system --gid resource-monitor --home-dir /nonexistent --shell /sbin/nologin resource-monitor
if [ -e "$settings_file" ] || [ -L "$settings_file" ]; then
    if [ ! -f "$settings_file" ] || [ -L "$settings_file" ]; then
        echo 'Existing settings.json must be a regular file and not a symbolic link' >&2
        exit 1
    fi
elif [ -e "$legacy_file" ] || [ -L "$legacy_file" ]; then
    if [ ! -f "$legacy_file" ] || [ -L "$legacy_file" ]; then
        echo 'Legacy thresholds.json must be a regular file and not a symbolic link' >&2
        exit 1
    fi
    if [ ! -e "$legacy_backup" ] && [ ! -L "$legacy_backup" ]; then
        cp -p --no-clobber -- "$legacy_file" "$legacy_backup" || exit 1
    fi
    cp -p --no-clobber -- "$legacy_file" "$migration_file" || exit 1
fi

%post
settings_file=/etc/resource-monitor/settings.json
migration_file=/etc/resource-monitor/.settings.json.migrate
if [ -e "$migration_file" ] || [ -L "$migration_file" ]; then
    if [ ! -f "$migration_file" ] || [ -L "$migration_file" ]; then
        echo 'Staged settings migration must be a regular file and not a symbolic link' >&2
        exit 1
    fi
    if [ ! -f "$settings_file" ] || [ -L "$settings_file" ]; then
        echo 'Installed settings.json must be a regular file and not a symbolic link; the staged original has been preserved' >&2
        exit 1
    fi
    mv -fT -- "$migration_file" "$settings_file" || exit 1
fi
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
%dir %attr(0750,root,resource-monitor) /etc/resource-monitor/secrets
%config(noreplace) /etc/resource-monitor/settings.json
%dir %attr(0750,resource-monitor,resource-monitor) /Monitor
/usr/share/resource-monitor
%dir /usr/share/doc/resource-monitor
%doc /usr/share/doc/resource-monitor/README.md
%license /usr/share/doc/resource-monitor/THIRD_PARTY_NOTICES.txt
