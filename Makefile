CPP := g++

LDFLAGS := -lldns -lmysqlpp
CPPFLAGS := -I/usr/include/mysql 
CPPFLAGS += -Wall

# if necessary, uncomment the following and set your libmysql++, libldns base
# path
#MYSQLDIR := /usr
#LDNSDIR := /usr
#MYSQLINCDIR := $(MYSQLDIR)/include
#MYSQLLIBDIR := $(MYSQLDIR)/lib
#LDNSINCDIR := $(LDNSDIR)/include
#LDNSLIBDIR := $(LDNSDIR)/lib

#CPPFLAGS += -I$(LDNSINCDIR) -I$(MYSQLINCDIR)
#LDFLAGS += -L$(LDNSLIBDIR) -L$(MYSQLLIBDIR)

DNSPERF := dnsperf

all: $(DNSPERF)

clean: 
	rm -f $(DNSPERF) *.o
