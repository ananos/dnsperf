CPP := g++
LDNSDIR := $(HOME)/local
LDNSINCDIR := $(LDNSDIR)/include
LDNSLIBDIR := $(LDNSDIR)/lib
CPPFLAGS := -I/usr/include/mysql -I/usr/include/mysql++ -I$(LDNSINCDIR)
CPPFLAGS += -Wall -Werror
LDFLAGS := -lmysqlpp -lmysqlclient -L$(LDNSLIBDIR) -lldns
DNSPERF := dnsperf 

all: $(DNSPERF)

clean: 
	rm -f $(DNSPERF) *.o
