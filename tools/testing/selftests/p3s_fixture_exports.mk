# SPDX-License-Identifier: GPL-2.0
# Do not turn a missing kernel export into a build failure or a false PASS.
p3s_exports := $(shell cat $(wildcard $(objtree)/Module.symvers $(KBUILD_EXTRA_SYMBOLS)) 2>/dev/null | awk '{print $$2}')
define p3s_export_fixture
ifeq ($$(filter-out $$(p3s_exports),$(2)),)
obj-m += $(1).o
else
$$(info SKIP $(1): missing exports $$(filter-out $$(p3s_exports),$(2)))
endif
endef
