
# consider adding ldmsd to bin_SCRIPTS if ldmsd script ever stabilizes
EXTRA_DIST=examples lsdate slurm-examples
EXTRA_DIST += ldms-csv-anonymize ldms-csv-export-sos
EXTRA_DIST += ovis-roll-over.py


if ENABLE_SCRIPTS
sbin_SCRIPTS= ldms-wrapper
bin_SCRIPTS = lsdate
bin_SCRIPTS += ldms-static-test.sh
bin_SCRIPTS += ovis-roll-over.py
bin_SCRIPTS += ldms-csv-anonymize
bin_SCRIPTS += ldms-csv-export-sos

DISTCLEANFILES = $(gen_scripts) $(sgen_scripts)
OPV=-$(PACKAGE_VERSION)

sgen_scripts = ldms-wrapper

gen_scripts = envldms.sh
gen_scripts += ldms_ban.sh
gen_scripts += ldms-plugins.sh
gen_scripts += ldms-py-subset_test.sh
gen_scripts += ldms-py-syslog.sh
gen_scripts += ldms-meminfo.sh
gen_scripts += ldms-py-varset.sh
gen_scripts += ldms-py-rename.sh
gen_scripts += ldms_local_opa2test.sh
gen_scripts += ldms-l2_test.sh

bin_SCRIPTS += $(gen_scripts)

testsets = install-serial
untestsets = uninstall-serial
if ENABLE_SLURMTEST
testsets += install-slurm
untestsets += uninstall-slurm
bin_SCRIPTS += pll-ldms-static-test.sh
endif

ovis-roll-over.py: ${srcdir}/ovis-roll-over.py
	cp ${srcdir}/ovis-roll-over.py .

lsdate: ${srcdir}/lsdate
	cp ${srcdir}/lsdate .

install-data-local: $(testsets)

install-serial:
	$(MKDIR_P) $(DESTDIR)$(docdir)$(OPV)/examples/static-test && \
	uf=$(DESTDIR)$(docdir)$(OPV)/examples/static-test/.uninst.ldms && \
	touch $$uf && \
	for f in $(srcdir)/examples/* ; do \
		if test "$$f" = "$(srcdir)/examples/README"; then \
			continue; \
		fi; \
		$(INSTALL_DATA) $$f $(DESTDIR)$(docdir)$(OPV)/examples/static-test; \
		echo $$(basename $$f) >> $$uf; \
	done
	$(INSTALL_DATA) $(srcdir)/examples/.canned $(DESTDIR)$(docdir)$(OPV)/examples/static-test

install-slurm:
	$(MKDIR_P) $(DESTDIR)$(docdir)$(OPV)/examples/slurm-test && \
	uf=$(DESTDIR)$(docdir)$(OPV)/examples/slurm-test/.uninst.ldms && \
	touch $$uf && \
	for f in $(srcdir)/slurm-examples/* ; do \
		if test "$$f" = "$(srcdir)/slurm-examples/README"; then \
			continue; \
		fi; \
		$(INSTALL_DATA) $$f $(DESTDIR)$(docdir)$(OPV)/examples/slurm-test; \
		echo $$(basename $$f) >> $$uf; \
	done
	$(INSTALL_DATA) $(srcdir)/slurm-examples/.canned $(DESTDIR)$(docdir)$(OPV)/examples/slurm-test

install-exec-hook:
	mv $(DESTDIR)/$(sbindir)/ldms-wrapper $(DESTDIR)/$(sbindir)/.ldms-wrapper

uninstall-serial:
	echo removing static-test files:
	while read f; do \
		echo $$f; \
		$(RM) $(DESTDIR)$(docdir)$(OPV)/examples/static-test/$$f; \
	done < $(DESTDIR)$(docdir)$(OPV)/examples/static-test/.uninst.ldms
	$(RM) $(DESTDIR)$(docdir)$(OPV)/examples/static-test/.canned
	$(RM) $(DESTDIR)$(docdir)$(OPV)/examples/static-test/.uninst.ldms

uninstall-slurm:
	echo removing slurm-test files:
	while read f; do \
		echo $$f; \
		$(RM) $(DESTDIR)$(docdir)$(OPV)/examples/slurm-test/$$f; \
	done < $(DESTDIR)$(docdir)$(OPV)/examples/slurm-test/.uninst.ldms
	$(RM) $(DESTDIR)$(docdir)$(OPV)/examples/slurm-test/.canned
	$(RM) $(DESTDIR)$(docdir)$(OPV)/examples/slurm-test/.uninst.ldms

uninstall-local: $(untestsets)
	echo removing other files:
	$(RM) $(DESTDIR)/$(sbindir)/.ldms-wrapper
endif
