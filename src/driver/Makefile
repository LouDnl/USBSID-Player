# psid64 code adapted for USBSID-Player
# Requires xa: https://www.floodgap.com/retrotech/xa/
# Requires bin2h: https://github.com/elnormous/bin2h

##
# Set build type here
# Release/Test/Debug
BUILDTYPE = Release

##
# Define tools to use
XA=xa
BIN2H=bin2h

##
# Set build type to Debu	g to export labels
# LABELS=
XAOPTS=-XMASM -c
LABELS=-G
EXTLABELS=-G
ifeq ($(BUILDTYPE),Debug)
LABELS:=-v -l psiddrv-labels.txt
EXTLABELS:=-v -l psidextdrv-labels.txt
endif

## Cleanup
.PHONY: clean
clean:
	$(RM) *.h *.txt *.o65
.PHONY: cleano65
cleano65:
	$(RM) *.o65

# Run it
all: psiddrv.h

## Targets
psiddrv.o65: psiddrv.a65
	$(XA) $(LABELS) $(XAOPTS) $(LABELS) -R psiddrv.a65 -o psiddrv.o65
psiddrv.h: psiddrv.o65
	$(BIN2H) -i psiddrv.o65 -n psid_driver -o psiddrv.h
