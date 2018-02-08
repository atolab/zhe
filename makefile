# Expect to be invoked either as:
#   "make <targets>"
# or as
#   "make -f .../makefile <targets>"
# so: the first word in $(MAKEFILE_LIST) will be the top-level
# makefile and therefore the $(dir) function will give us the relative
# directory. If it is "./" assume we're building inside the source dir
# and don't need to fiddle with the vpaths, otherwise ./ assume we're
# outside the source dir and set the vpath to point to the sources
SRCDIR := $(patsubst %/,%,$(dir $(firstword $(MAKEFILE_LIST))))
SUBDIRS = src test
vpath %.c $(SUBDIRS:%=$(SRCDIR)/%)
vpath %.h $(SUBDIRS:%=$(SRCDIR)/%)

TARGETS = roundtrip throughput
ZHE_PLATFORM := platform-udp.c
ZHE := $(notdir $(wildcard $(SRCDIR)/src/*.c)) $(ZHE_PLATFORM)

OPT = -O3
CFLAGS = $(OPT) -g -Wall $(SUBDIRS:%=-I$(SRCDIR)/%)
LDFLAGS = $(OPT)

SRC_roundtrip = roundtrip.c testlib.c $(ZHE)
SRC_throughput = throughput.c testlib.c $(ZHE)

.PHONY: all clean zz
.PRECIOUS: %.o
.SECONDEXPANSION:
%: %.o
%: %.c

all: $(TARGETS)

$(TARGETS): $$(patsubst %.c, %.o, $$(SRC_$$@))

%:
	$(CC) $(LDFLAGS) $^ -o $@

%.d: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -M $< -o $@

clean: ; rm -f $(TARGETS) *.[od]

zz:
	@echo $(ZHE)

ifneq ($(MAKECMDGOALS),clean)
  -include $(patsubst %.c, %.d, $(sort $(foreach x, $(TARGETS), $(SRC_$x))))
endif
