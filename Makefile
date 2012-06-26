CPP := g++
LDNSDIR := $(HOME)/local
LDNSINCDIR := $(LDNSDIR)/include
LDNSLIBDIR := $(LDNSDIR)/lib
CPPFLAGS := -I/usr/include/mysql -I/usr/include/mysql++ -I$(LDNSINCDIR)
LDFLAGS := -lmysqlpp -lmysqlclient -L$(LDNSLIBDIR) -lldns
DNSPERF := dnsperf 

all: $(DNSPERF)

%: %.o
	$(CPP) $(LDFLAGS) $^ -o $@

%.o: %.cpp
	$(CPP) -c -o $@ $< $(CPPFLAGS)

clean: 
	rm -f $(DNSPERF) *.o
