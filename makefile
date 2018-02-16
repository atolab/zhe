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
SUBDIRS = src example/platform example/configs/p2p example/throughput example/roundtrip
vpath %.c $(SUBDIRS:%=$(SRCDIR)/%)
vpath %.h $(SUBDIRS:%=$(SRCDIR)/%)

TARGETS = bin/roundtrip bin/throughput bin/psrid
ZHE_PLATFORM := platform-udp.c
ZHE := $(notdir $(wildcard $(SRCDIR)/src/*.c)) $(ZHE_PLATFORM)

OPT = #-O2
CFLAGS = $(OPT) -g -Wall $(SUBDIRS:%=-I$(SRCDIR)/%)
LDFLAGS = $(OPT)

SRC_roundtrip = roundtrip.c zhe-util.c $(ZHE)
SRC_throughput = throughput.c zhe-util.c $(ZHE)
SRC_psrid = psrid.c zhe-util.c $(ZHE)

.PHONY: all clean zz test-configs
.PRECIOUS: %.o %/.STAMP
.SECONDEXPANSION:
%: %.o
%: %.c
%.o: %.c

all: $(TARGETS)

$(TARGETS): $$(patsubst %.c, gen/%.o, $$(SRC_$$(notdir $$@)))

test-configs: $(addprefix test/build.configs/throughput-, $(notdir $(wildcard example/configs/*)))

%/.STAMP:
	[ -d $* ] || mkdir -p $*
	touch $@

test/build.configs/throughput-%: test/build.configs/.STAMP $(SRC_throughput) 
	$(CC) -Iexample/configs/$* $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(wordlist 2, 999, $^) -o $@

bin/%: | bin/.STAMP
	$(CC) $(LDFLAGS) $^ -o $@

gen/%.o: %.c gen/.STAMP
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

gen/psrid.o: test/psrid.c gen/.STAMP
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

gen/%.d: %.c gen/.STAMP
	$(CC) $(CPPFLAGS) $(CFLAGS) -M $< -o $@

clean: ; rm -rf $(TARGETS) gen

zz:
	@echo $(ZHE)

ifneq ($(MAKECMDGOALS),clean)
  -include $(patsubst %.c, gen/%.d, $(sort $(foreach x, $(TARGETS), $(SRC_$(notdir $x)))))
endif
