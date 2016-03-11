## Process this file with automake to produce Makefile.in

SUBDIRS = src scripts doc man
DIST_SUBDIRS = $(SUBDIRS)

dist_doc_DATA = \
	README \
	LICENSE \
	GPL-3.0.txt \
	AUTHORS \
	ChangeLog \
	TODO

EXTRA_DIST = \
	aclocal.m4 \
	configure \
	ltmain.sh \
	jamrouter.spec


#install-data-local:
#	@$(NORMAL_INSTALL)


#uninstall-local:
#	@$(NORMAL_UNINSTALL)


uninstall-hook:
	test -d $(DESTDIR)$(pkgdatadir) && rmdir --ignore-fail-on-non-empty $(DESTDIR)$(pkgdatadir)
	test -d $(DESTDIR)$(docdir) && rmdir $(DESTDIR)$(docdir)


distclean-local:
	rm -f *~
	rm -rf autom4te.cache


jamrouter-$(PACKAGE_VERSION).tar.gz: dist


jamrouter-version:
	scripts/version-sh


jamrouter.spec: jamrouter-version jamrouter.spec.in


rpm: jamrouter-$(PACKAGE_VERSION).tar.gz jamrouter.spec
	rpmbuild=$$(mktemp -d -t rpmbuild-jamrouter.XXXXXXX); src=$$(pwd); \
	cp jamrouter-$(PACKAGE_VERSION).tar.gz "$$rpmbuild"; \
	cp jamrouter.spec $$rpmbuild/jamrouter.spec; \
	pushd "$$rpmbuild" && \
	rpmbuild \
		--define "_topdir $$PWD" \
		--define "_sourcedir $$PWD" \
		--define "_specdir $$PWD" \
		--define "_srcrpmdir $$PWD" \
		--define "_rpmdir $$PWD" \
		-ba jamrouter.spec && \
	popd; \
	mv "$$rpmbuild"/$$(arch)/*.rpm . &&  mv "$$rpmbuild"/*.src.rpm . && \
		rm -rf "$$rpmbuild" && ls -1 *.rpm

