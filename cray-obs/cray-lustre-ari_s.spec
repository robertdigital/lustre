%define vendor_name lustre
%define _version %(if test -s "%_sourcedir/_version"; then cat "%_sourcedir/_version"; else echo "UNKNOWN"; fi)
%define flavor cray_ari_s
%define intranamespace_name %{vendor_name}-%{flavor}
%define source_name %{vendor_namespace}-%{vendor_name}-%{_version}
%define branch trunk
%define _lnet_version %(echo "%{_version}" | awk -F . '{printf("%s.%s", $1, $2)}')

# Override _prefix to avoid installing into Cray locations under /opt/cray/
%define _prefix /usr
%define kversion %(make -s -C /usr/src/linux-obj/%{_target_cpu}/%{flavor} kernelrelease)

%bcond_with server
%if 0%{?sle_version} == 150000
# Enable server builds for SLES15
%define with_server 1
%define config_server --enable-server
%else
%define config_server --disable-server
%endif

BuildRequires: cray-gni-devel
BuildRequires: cray-gni-headers
BuildRequires: cray-gni-headers-private
BuildRequires: cray-krca-devel
BuildRequires: lsb-cray-hss-devel
BuildRequires: kernel-source
BuildRequires: kernel-syms
BuildRequires: zlib-devel
BuildRequires: module-init-tools
BuildRequires: pkgconfig
BuildRequires: libtool
BuildRequires: libyaml-devel
%if 0%{?sle_version} <= 120000
BuildRequires: ofed-devel
%endif
BuildConflicts: post-build-checks
Group: System/Filesystems
License: GPL
Name: %{namespace}-%{intranamespace_name}
Release: %{release}
Requires: module-init-tools
Summary: Lustre File System for Aries Service Nodes running CLE
Version: %{_version}
Source: %{source_name}.tar.bz2
Source99: cray-lustre-rpmlintrc
URL: %url
BuildRoot: %{_tmppath}/%{name}-%{version}-root
Provides: %{name}_rhine

%description
Kernel modules and userspace tools needed for a Lustre client on XC SLES-based
service nodes running the CLE release.
Compiled for kernel: %{kversion}

%package -n cray-lustre-cray_ari_s-%{_lnet_version}-devel
Group: Development/Libraries
License: GPL
Summary: Cray Lustre Header files
Provides: cray-lnet-%{_lnet_version}-devel

%description -n cray-lustre-cray_ari_s-%{_lnet_version}-devel
Development files for building against Lustre library.
Includes headers, dynamic, and static libraries.
Compiled for kernel: %{kversion}

%if %{with server}
%package resource-agents
Summary: HA Resuable Cluster Resource Scripts for Lustre
Group: System Environment/Base
Requires: resource-agents

%description resource-agents
A set of scripts to operate Lustre resources in a High Availablity
environment for both Pacemaker and rgmanager.
%endif

%prep
%incremental_setup -q -n %{source_name}

%build
echo "LUSTRE_VERSION = %{_version}" > LUSTRE-VERSION-FILE
%define version_path %(basename %url)
%define date %(date +%%F-%%R)
%define lustre_version %{_version}-%{branch}-%{release}-%{build_user}-%{version_path}-%{date}

# Sets internal kgnilnd build version
export SVN_CODE_REV=%{lustre_version}

if [ "%reconfigure" == "1" -o ! -x %_builddir/%{source_name}/configure ];then
	chmod +x autogen.sh
	./autogen.sh
fi

syms="$(pkg-config --variable=symversdir cray-gni)/%{flavor}/Module.symvers"
syms="$syms $(pkg-config --variable=symversdir cray-krca)/%{flavor}/Module.symvers"

export GNICPPFLAGS=$(pkg-config --cflags cray-gni cray-gni-headers cray-krca lsb-cray-hss)
if [ -d /usr/src/kernel-modules-ofed/%{_target_cpu}/%{flavor} ]; then
	O2IBPATH=/usr/src/kernel-modules-ofed/%{_target_cpu}/%{flavor}
	syms="$syms /usr/src/kernel-modules-ofed/%{_target_cpu}/%{flavor}/Modules.symvers"
elif [ -d /usr/src/ofed/%{_target_cpu}/%{flavor} ]; then
	O2IBPATH=/usr/src/ofed/%{_target_cpu}/%{flavor}
else
	O2IBPATH=yes
fi

export KBUILD_EXTRA_SYMBOLS=${syms}

HSS_FLAGS=$(pkg-config --cflags lsb-cray-hss)
CFLAGS="%{optflags} -Werror -fno-stack-protector $HSS_FLAGS"

if [ "%reconfigure" == "1" -o ! -f %_builddir/%{source_name}/Makefile ];then
	%configure --disable-checksum \
		--enable-gni \
		%{config_server} \
		--with-linux-obj=/usr/src/linux-obj/%{_target_cpu}/%{flavor} \
		--with-o2ib=${O2IBPATH} \
		--with-systemdsystemunitdir=%{_unitdir} \
		--with-obd-buffer-size=16384
fi
%{__make} %_smp_mflags

%install
# Sets internal kgnilnd build version
export SVN_CODE_REV=%{lustre_version}

make DESTDIR=${RPM_BUILD_ROOT} install

for header in api.h lib-lnet.h lib-types.h
do
	%{__install} -D -m 0644 lnet/include/lnet/${header} %{buildroot}/%{_includedir}/lnet/${header}
done

for header in libcfs_debug.h lnetctl.h lnetst.h libcfs_ioctl.h lnet-dlc.h \
	      lnet-types.h nidstr.h
do
	%{__install} -D -m 0644 lnet/include/uapi/linux/lnet/${header} %{buildroot}/%{_includedir}/uapi/linux/lnet/${header}
done

for header in libcfs.h util/list.h curproc.h bitmap.h libcfs_private.h libcfs_cpu.h \
	      libcfs_prim.h libcfs_string.h libcfs_workitem.h \
	      libcfs_hash.h libcfs_heap.h libcfs_fail.h libcfs_debug.h range_lock.h
do
	%{__install} -D -m 0644 libcfs/include/libcfs/${header} %{buildroot}/%{_includedir}/libcfs/${header}
done

for header in linux-fs.h linux-mem.h linux-time.h linux-cpu.h linux-crypto.h \
	      linux-misc.h
do
	%{__install} -D -m 0644 libcfs/include/libcfs/linux/${header} %{buildroot}/%{_includedir}/libcfs/linux/${header}
done

%{__install} -D -m 0644 lustre/include/interval_tree.h %{buildroot}/%{_includedir}/interval_tree.h

%define cfgdir %{_includedir}/lustre/%{flavor}
for f in cray-lustre-api-devel.pc cray-lustre-cfsutil-devel.pc \
	 cray-lustre-ptlctl-devel.pc cray-lnet.pc
do
	eval "sed -i 's,@includedir@,%{_includedir},' cray-obs/${f}"
	eval "sed -i 's,@libdir@,%{_libdir},' cray-obs/${f}"
	eval "sed -i 's,@symversdir@,%{_datadir}/symvers,' cray-obs/${f}"
	eval "sed -i 's,@PACKAGE_VERSION@,%{_version},' cray-obs/${f}"
	eval "sed -i 's,@cfgdir@,%{cfgdir},' cray-obs/${f}"
	install -D -m 0644 cray-obs/${f} $RPM_BUILD_ROOT%{_pkgconfigdir}/${f}
done

# Install Module.symvers and config.h for the lnet devel package
%{__install} -D -m 0644 ${PWD}/Module.symvers %{buildroot}/%{_datadir}/symvers/%{_arch}/%{flavor}/Module.symvers
%{__install} -D -m 0644 config.h %{buildroot}/%{cfgdir}/config.h

# Install module directories and files
%{__sed} -e 's/@VERSION@/%{version}-%{release}/g' cray-obs/version.in > .version
%{__sed} -e 's,@pkgconfigdir@,%{_pkgconfigdir},g' cray-obs/module.in > module
%{__install} -D -m 0644 .version %{buildroot}/%{_name_modulefiles_prefix}/.version
%{__install} -D -m 0644 module %{buildroot}/%{_release_modulefile}

rm -f $RPM_BUILD_ROOT%{_libdir}/liblustreapi.la
rm -f $RPM_BUILD_ROOT%{_libdir}/liblnetconfig.la

%files
%defattr(-,root,root)
/sbin/mount.lustre
/etc/udev
/lib/modules/%{kversion}
%{_sbindir}/*
%{_bindir}/*
%{_mandir}/*
%{_unitdir}/lnet.service
%{_includedir}/lustre
%{_includedir}/linux/lnet
%{_includedir}/linux/lustre
%{_libdir}/liblustreapi.a
%{_libdir}/liblustreapi.so*
%{_libdir}/liblnetconfig.a
%{_libdir}/liblnetconfig.so*
%{_pkgconfigdir}/cray-lustre-api-devel.pc
%{_pkgconfigdir}/cray-lustre-cfsutil-devel.pc
%{_pkgconfigdir}/cray-lustre-ptlctl-devel.pc
%dir %{_libdir}/lustre
%{_libdir}/lustre/tests
%{_modulefiles_prefix}
%if %{with server}
%{_libdir}/lustre/mount_osd_ldiskfs.so
%{_libdir}/libiam.a
%exclude /etc/ha.d/resource.d/Lustre.ha_v2
%exclude %{_libexecdir}/lustre/haconfig
%exclude %{_libexecdir}/lustre/lc_common
%endif
%exclude /etc/lustre/perm.conf
%exclude /etc/modprobe.d/ko2iblnd.conf
%exclude /etc/lnet.conf
%exclude /etc/lnet_routes.conf
%exclude %{_mandir}/man5
%exclude %{_mandir}/man8/lhbadm.8.gz
%exclude %{_pkgconfigdir}/cray-lnet.pc

%files -n cray-lustre-cray_ari_s-%{_lnet_version}-devel
%defattr(-,root,root)
%dir %{_datadir}/symvers
%dir %{_datadir}/symvers/%{_arch}
%dir %{_datadir}/symvers/%{_arch}/%{flavor}
%attr (644,root,root) %{_datadir}/symvers/%{_arch}/%{flavor}/Module.symvers
%{_pkgconfigdir}/cray-lnet.pc
%{_includedir}/*

%if %{with server}
%files resource-agents
%defattr(0755,root,root)
%{_prefix}/lib/ocf/resource.d/lustre/
%endif

%post
DEPMOD_OPTS=""
if [ -f /boot/System.map-%{kversion} ]; then
	DEPMOD_OPTS="-F /boot/System.map-%{kversion}"
fi

depmod -a ${DEPMOD_OPTS} %{kversion}

%postun
DEPMOD_OPTS=""
if [ -f /boot/System.map-%{kversion} ]; then
	DEPMOD_OPTS="-F /boot/System.map-%{kversion}"
fi

depmod -a ${DEPMOD_OPTS} %{kversion}

%clean
%clean_build_root