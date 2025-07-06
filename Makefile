# Main Makefile - delegates to src/ directory
.PHONY: all clean install dkms-install dkms-uninstall load unload reload

all:
	$(MAKE) -C src all

clean:
	$(MAKE) -C src clean

install:
	$(MAKE) -C src install

# DKMS specific targets
dkms-install:
	cd dkms && sudo ./install.sh

dkms-uninstall:
	cd dkms && sudo ./uninstall.sh

# Helper targets
load:
	./loader.sh load

unload:
	./loader.sh unload

reload:
	./loader.sh unload
	$(MAKE) -C src all
	./loader.sh load
