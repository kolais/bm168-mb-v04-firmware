SHELL := /bin/sh
PYTHON ?= python3
# The build's Docker image and its scratch volume; the test scripts use the same two.
IMAGE ?= autotop-bm168-build:1
VOLUME ?= autotop-bm168-work
export IMAGE VOLUME
ROOT := $(CURDIR)
DOCKER_RUN = docker run --rm --platform linux/amd64 --network none \
  --mount "type=bind,src=$(ROOT),dst=/project,readonly" \
  --mount "type=bind,src=$(ROOT)/dist,dst=/out" \
  --mount "type=volume,src=$(VOLUME),dst=/work" $(IMAGE)

.PHONY: help fetch builder apps firmware recovery recovery-readonly recovery-capture recovery-writer recovery-wifi recovery-spike plana-b plana-b-state wifi-fetch shell preview sdk-fetch ecc-check design ui-snap ui-test app-bundle app-slot-test system-test rescue-image rescue-test keys session rescue
help:
	@printf '%s\n' \
	  'make apps      Build static ARM UI and application overlay bundle' \
	  'make firmware  Build modified vendor rootfs as UBIFS and UBI images' \
	  'make recovery  Build a RAM console probe with no flash drivers' \
	  'make recovery-readonly Build a RAM probe with read-only USB/NAND access' \
	  'make recovery-capture  Build a read-only NAND acquisition image (writable capture stick)' \
	  'make recovery-writer WINDOW=START:SIZE  Build the bounded NAND writer for one window' \
	  'make recovery-wifi  Build the RAM Wi-Fi probe (read-only NAND, RTL8821CS on USB)' \
	  'make wifi-fetch Verify or reproduce the pinned RTL8821CS driver archive' \
	  'make recovery-spike Build the Plan A GPU/display RAM spike (modules and stock squashfs on USB)' \
	  'make ecc-check Check the stock ECC model against private raw captures' \
	  'make preview   Run editable UI natively with SDL2 (macOS/Linux)' \
	  'make design    Regenerate design/tokens.css and launcher/ui/tokens.h from design/tokens.json' \
	  'make ui-snap   Test the launcher navigation and draw its screens natively into build/snap/out/' \
	  'make ui-test   Build the launcher natively; run the navigation tests and the launcherd scenarios' \
	  'make shell     Open the Linux build environment' \
	  'make sdk-fetch Verify or reproduce the pinned sanitised BSP archive'

fetch:
	$(PYTHON) tools/build.py fetch

builder:
	docker build --platform linux/amd64 -t $(IMAGE) -f tools/Dockerfile .

apps: fetch builder
	@mkdir -p dist
	$(DOCKER_RUN) python3 tools/build.py apps

firmware: fetch builder
	@mkdir -p dist
	$(DOCKER_RUN) python3 tools/build.py firmware

recovery: fetch builder sdk-fetch
	@mkdir -p dist
	$(DOCKER_RUN) python3 tools/recovery.py

recovery-readonly: fetch builder sdk-fetch
	@mkdir -p dist
	$(DOCKER_RUN) python3 tools/recovery.py --readonly-storage

recovery-capture: fetch builder sdk-fetch
	@mkdir -p dist
	$(DOCKER_RUN) python3 tools/recovery.py --capture

# First restore test: kernel block 0 only. Every other window is a separate build and review.
WINDOW ?= 0x160000:0x20000
recovery-writer: fetch builder sdk-fetch
	@mkdir -p dist
	$(DOCKER_RUN) python3 tools/recovery.py --writer --window $(WINDOW)

recovery-wifi: fetch builder sdk-fetch wifi-fetch
	@mkdir -p dist
	$(DOCKER_RUN) python3 tools/recovery.py --wifi

recovery-spike: fetch builder sdk-fetch wifi-fetch
	@mkdir -p dist
	$(DOCKER_RUN) python3 tools/recovery.py --spike

plana-b: fetch builder sdk-fetch wifi-fetch
	@mkdir -p dist
	$(DOCKER_RUN) python3 tools/recovery.py --plana-b

plana-b-state: fetch builder sdk-fetch wifi-fetch
	@mkdir -p dist
	$(DOCKER_RUN) python3 tools/recovery.py --plana-b --state

wifi-fetch:
	$(PYTHON) tools/build.py wifi-fetch

# The ECC code against a raw capture of the builder's own unit (pages with their spare areas).
CAPTURE ?= backups/2026-11-13
ecc-check:
	$(PYTHON) tools/nand_ecc.py verify $(CAPTURE)/mtd[1-578]-*.raw-oob.bin $(CAPTURE)/mtd12-*.raw-oob.bin

shell: builder
	@mkdir -p dist
	$(DOCKER_RUN) bash

preview: fetch
	$(PYTHON) tools/build.py preview

# A signed app bundle from the last image build (recovery/plan-a/app-slot):
# make app-bundle VERSION=2 [KEY=...]. The version must be above the one the
# unit runs; recovery/plan-a/app-version is the one built into images.
KEYS ?= $(HOME)/.config/bm168-mb-v04
KEY ?= $(KEYS)/update-signing-key.pem

# The builder's own keys: private halves in $(KEYS), public halves in local/keys/ (tools/make_keys.sh).
keys:
	KEYS=$(KEYS) tools/make_keys.sh
# The rescue stick's software (recovery/plan-a/init-rescue): the kernel of the
# last build and a rootfs partition image (tools/plana_b_image.py; it holds
# the unit's private session files, so it lives under backups/) as one file.
# make rescue-image ROOTFS=backups/.../PARTITION.IMG [PLAN_ONLY=1] [NOTE=...]
rescue-image:
	$(PYTHON) tools/rescue_image.py --kernel dist/plana-b-state/plana-b.bin --rootfs $(ROOTFS) \
		--out dist/RESCUE.IMG $(if $(PLAN_ONLY),--plan-only) --note "$(NOTE)"

rescue-test:
	tools/rescue_test.sh

# The unit's own session files, once, from the captures in local/stock/ (tools/session_seed.py).
# Files already in local/state/ are kept.
session:
	@mkdir -p dist local/state
	$(DOCKER_RUN) python3 tools/session_seed.py /project/local/stock/rootfs.ubi /project/local/stock/userdata.ubi /out/session
	cp -n dist/session/DATA.TAR dist/session/GOC.TAR dist/session/PLANA.CFG local/state/ || true
	rm -rf dist/session

# The two files of the rescue stick, from the last `make plana-b-state` and local/state/:
# dist/rescue/BOOT.BIN (the installer) and dist/rescue/RESCUE.IMG (kernel and rootfs).
# RESCUE.IMG holds the unit's session files: it is as private as they are.
rescue:
	@mkdir -p dist/rescue
	$(DOCKER_RUN) python3 tools/plana_b_image.py --squashfs /out/plana-b-state/stock-rootfs.sqfs \
		--state-dir /project/local/state --out /out/rescue/rootfs
	$(PYTHON) tools/rescue_image.py --kernel dist/plana-b-state/plana-b.bin --rootfs dist/rescue/rootfs/PARTITION.IMG \
		--out dist/rescue/RESCUE.IMG $(if $(PLAN_ONLY),--plan-only) --note "$(NOTE)"
	cp dist/plana-b-state/rescue.bin dist/rescue/BOOT.BIN

app-slot-test:
	tools/app_slot_test.sh

system-test:
	tools/system_test.sh

app-bundle:
	$(PYTHON) tools/app_bundle.py --tree dist/plana-b-state/app --version $(VERSION) \
		--system $$(cat recovery/plan-a/system-level) --key $(KEY) --check local/keys/update-key.pem \
		--out dist/autotop-app-$(VERSION).bundle

design:
	$(PYTHON) tools/design_tokens.py
	$(PYTHON) tools/design_sounds.py

ui-snap: fetch
	$(PYTHON) tools/build.py ui-snap

ui-test: fetch
	$(PYTHON) tools/build.py ui-test

sdk-fetch:
	$(PYTHON) tools/build.py sdk-fetch
