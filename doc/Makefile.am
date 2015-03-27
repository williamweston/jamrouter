## Process this file with automake to produce Makefile.in

SUBDIRS = latency-tests

dist_docs = \
	juno-106.txt \
	midi-clock.txt \
	release-checklist.txt

dist_doc_DATA = ${dist_docs}

EXTRA_DIST = ${dist_docs}


uninstall-hook:
	for subdir in $(SUBDIRS); do \
		test -d $(DESTDIR)$(docdir)/$$subdir && rmdir $(DESTDIR)$(docdir)/$$subdir; \
	done
