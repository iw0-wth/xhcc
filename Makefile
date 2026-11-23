# Host build of both WASI applications. Tool paths are overridable.
PYTHON ?= python
WASI_SDK_PATH ?= D:/wasi-sdk-34.0-x86_64-windows
SD_ROOT ?= ../sd/xhcc
.PHONY: all xhcc xhccm test deploy-sd test-sd resources
all:
	$(PYTHON) tools/build.py --app both --sdk "$(WASI_SDK_PATH)"
xhcc:
	$(PYTHON) tools/build.py --app xhcc --sdk "$(WASI_SDK_PATH)"
xhccm:
	$(PYTHON) tools/build.py --app xhccm --sdk "$(WASI_SDK_PATH)"
test:
	$(PYTHON) tools/test.py
	$(PYTHON) tests/test_make.py
deploy-sd: all
	$(PYTHON) tools/deploy_sd.py --sd "$(SD_ROOT)"
test-sd:
	$(PYTHON) tests/test_sd.py --sd "$(SD_ROOT)"
resources:
	$(PYTHON) tools/resource_report.py --self-test --json build/reports/resources.json --markdown build/reports/resources.md
