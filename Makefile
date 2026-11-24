# Four sibling WASI applications. Tool paths are overridable.
PYTHON ?= python
WASI_SDK_PATH ?= D:/wasi-sdk-34.0-x86_64-windows
SD_ROOT ?= ../sd/xhcc
.PHONY: all xhcc xhccm xhccn xhlinker test deploy-sd test-sd resources nopy
all:
	$(PYTHON) tools/build.py --app all-apps --sdk "$(WASI_SDK_PATH)"
xhcc xhccm xhccn xhlinker:
	$(PYTHON) tools/build.py --app $@ --sdk "$(WASI_SDK_PATH)"
nopy:
	$(MAKE) -f Makefile.nopy all
test:
	$(PYTHON) tools/test.py
	$(PYTHON) tests/test_make.py
deploy-sd: all
	$(PYTHON) tools/deploy_sd.py --sd "$(SD_ROOT)"
test-sd:
	$(PYTHON) tests/test_sd.py --sd "$(SD_ROOT)"
	$(PYTHON) tests/test_resource_budget.py --sd "$(SD_ROOT)"
resources:
	$(PYTHON) tools/resource_report.py --self-test --json build/reports/resources.json --markdown build/reports/resources.md
