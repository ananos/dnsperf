/*
 * dnsperf.cpp -- Copyright (c) Anastassios Nanos 2012
 * based on ldns and mysql++-doc examples.
 *
 * example solution to the following challenge:
 * Title: Tracking DNS performance to top sites
 *
 * Importance: Such performance numbers to top sites can be used as benchmarks
 * for others to compare to.
 *
 * Description: Write a c++ program on Linux/BSD(Mac) that periodically sends
 * DNS queries to the nameservers of the top 10 Alexa domains and stores the
 * latency values in a mysql table. The frequency of queries should be
 * specified by the user on command line. The program needs to make sure it
 * doesnt hit the DNS cache while trying to query for a site and should use a
 * random string prepended to a domain. E,g. to query foo.bar, make sure to
 * prepend a random string, e.g. 1234xpto.foo.bar.
 *
 * Besides the timeseries values, the code needs to keep track in db stats per domain about :
 * - the average query times
 * - standard deviation of DNS query times
 * - number of queries made so far
 * - time stamp of first query made per domain and last query made
 *
 * Refs:
 * a. Mysql lib, use mysql++:
 * http://tangentsoft.net/mysql++/
 * b. DNS lib, us ldns:
 * http://www.nlnetlabs.nl/projects/ldns/
 *
 */

#include <iostream>
#include <iomanip>
#include <getopt.h>

#include <mysql++.h>
#include <time.h>
#include <ldns/ldns.h>

using namespace std;

#define HOSTNAME_MAX 50
char *dnsperf_hostname = NULL;
uint8_t dnsperf_resetdb = 0;
uint8_t dnsperf_randhost_idx = 0;
uint8_t dnsperf_dumpdb = 0;
unsigned long dnsperf_freq = 1;
const char *dnsperf_dbhostname = "localhost";
const char *dnsperf_dbname = "mysql_cpp_data";
const char *dnsperf_dbuser = "root";
const char *dnsperf_dbpass = "";
const char *dnsperf_valtable = "dnsqueries";
const char *dnsperf_domaintable = "domains";
int dnsperf_initdb(void);
ldns_resolver *build_resolver(const char *domainname,
			      ldns_rr_list ** query_results);
unsigned long resolve(const char *domaintoquery, ldns_resolver * actual_res);

int dnsperf_stats(char *domain);
int parse_cmdline(int argc, char **argv)
{
	int aflag = 0;
	int bflag = 0;
	char *cvalue = NULL;
	int index;
	int c;

	opterr = 0;

	while ((c = getopt(argc, argv, "vrh:u:p:m:c:t:d:f:")) != -1)
		switch (c) {
		case 'f':
			dnsperf_freq = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			dnsperf_dumpdb = 1;
			break;
		case 'r':
			dnsperf_resetdb = 1;
			break;
		case 't':
			dnsperf_valtable = strdup(optarg);
			break;
		case 'd':
			dnsperf_domaintable = strdup(optarg);
			break;
		case 'h':
			dnsperf_hostname = strdup(optarg);
			break;
		case 'm':
			dnsperf_dbname = strdup(optarg);
			break;
		case 'u':
			dnsperf_dbuser = strdup(optarg);
			break;
		case 'p':
			dnsperf_dbpass = strdup(optarg);
			break;
		case 'c':
			dnsperf_dbhostname = strdup(optarg);
			break;
		case '?':
			if (optopt == 'h')
				fprintf(stderr,
					"Option -%c requires an argument.\n",
					optopt);
			else if (isprint(optopt))
				fprintf(stderr, "Unknown option `-%c'.\n",
					optopt);
			else
				fprintf(stderr,
					"Unknown option character `\\x%x'.\n",
					optopt);
			return 1;
		default:
			abort();
		}

	return 0;
}

int main(int argc, char *argv[])
{
	unsigned long timevalue;

	/* Init rand() */
	srand((unsigned)time(NULL));

	if (parse_cmdline(argc, argv)) {
		cout << "Error parsing arguments" << endl;
		exit(1);
	}

	if (dnsperf_resetdb) {
		/* Reset DB values */
		if (dnsperf_initdb()) {
			cout << "Database init failed, exiting" << endl;
			exit(1);
		}
	}

	/* Connect to the database */
	mysqlpp::Connection conn((bool) false);
	cout << "Connecting to MYSQL://" << dnsperf_dbuser << "@" <<
	    dnsperf_dbhostname << "/" << dnsperf_dbname << endl;
	if (conn.connect(dnsperf_dbname, dnsperf_dbhostname, dnsperf_dbuser,
			 dnsperf_dbpass)) {
		int i = 0;

		// Retrieve the sample stock table set up by resetdb
		mysqlpp::Query query_domains = conn.query();
		query_domains << "select * from %6:table";
		query_domains.parse();
		query_domains.template_defaults["table"] = dnsperf_domaintable;
		if (dnsperf_dumpdb)
			cout << query_domains << endl;
		mysqlpp::StoreQueryResult res_domains = query_domains.store();

		if (dnsperf_dumpdb) {
			// Display results
			if (res_domains) {
				// Display header
				cout.setf(ios::left);
				cout <<
				    setw(10) << "Rank" <<
				    setw(10) << "Domain" << endl << endl;

				// Get each row in result set, and print its contents
				for (size_t i = 0; i < res_domains.num_rows();
				     ++i) {
					cout << setw(5) <<
					    res_domains[i]["rank"] << ' ' <<
					    setw(30) << res_domains[i]["domain"]
					    << ' ' << endl;
				}
			} else {
				cerr << "Failed to get " << dnsperf_valtable <<
				    " table: " << query_domains.error() << endl;
				return 1;
			}
		}

		if (!dnsperf_hostname) {
			char *tmp = (char *)malloc(HOSTNAME_MAX);
			snprintf(tmp, 7, "foo%d", rand() % 1024);
			dnsperf_hostname = tmp;
			dnsperf_randhost_idx = strlen(dnsperf_hostname);
		}
		while (1) {
			for (i = 0; i < res_domains.num_rows(); i++) {
				const char *domain =
				    res_domains[i]["domain"].c_str();
				char *nameserver;
				ldns_rdf *ns_name;
				ldns_rr_list *iplist;
				ldns_resolver *actual_res;
				ldns_rr_list *query_results;
				int j = 0;

				if (dnsperf_randhost_idx)
					sprintf(dnsperf_hostname +
						dnsperf_randhost_idx, "%s\n",
						domain);

				actual_res =
				    build_resolver(domain, &query_results);

				for (j = 0;
				     j < ldns_rr_list_rr_count(query_results);
				     j++) {
					ldns_rdf *ns;
					ldns_resolver *resolver;
					resolver = ldns_resolver_new();
					ns_name =
					    ldns_rr_rdf(ldns_rr_list_rr
							(query_results, j), 0);
					iplist =
					    ldns_get_rr_list_addr_by_name
					    (actual_res, ns_name,
					     LDNS_RR_CLASS_IN, 0);
					(void)
					    ldns_resolver_push_nameserver_rr_list(resolver, iplist);
					if (!ns_name) {
						printf
						    ("error, ns_name is null\n");
					} else {
						char *test =
						    ldns_rdf2str(ns_name);
						(nameserver) = test;
					}
					timevalue =
					    resolve(dnsperf_hostname, resolver);
					if (!timevalue)
						continue;
					ldns_resolver_free(resolver);

					try {
						mysqlpp::Query query =
						    conn.query();
						if (dnsperf_dumpdb)
							cout <<
							    "Updating mysql table "
							    << dnsperf_valtable
							    << " ..." << endl;

						query <<
						    "insert into %6:table values "
						    <<
						    "(%0q, %1q, %2q, %3q, %4q:desc)";
						query.parse();

						query.
						    template_defaults["table"] =
						    dnsperf_valtable;
						query.
						    template_defaults["desc"] =
						    mysqlpp::null;

						if (dnsperf_dumpdb)
							cout << "Populating " <<
							    dnsperf_valtable <<
							    " table..." <<
							    flush;
						char date[20];
						time_t tm;
						struct tm *tmp;
						tm = time(NULL);
						tmp = localtime(&tm);
						strftime(date, 20,
							 "%Y-%m-%d %X", tmp);
						query.execute(domain, timevalue,
							      date, nameserver);

						if (dnsperf_dumpdb) {
							cout << "inserted " << 1
							    << " row." << endl;
							cout << domain << " " <<
							    timevalue << " " <<
							    date << " " <<
							    nameserver << endl;
						}
					}
					catch(const mysqlpp::BadQuery & er) {
						cerr << endl << "Query error: "
						    << er.what() << endl;
						return 1;
					}
					catch(const mysqlpp::BadConversion & er) {
						cerr << endl <<
						    "Conversion error: " << er.
						    what() << endl <<
						    "\tretrieved data size: " <<
						    er.retrieved <<
						    ", actual size: " << er.
						    actual_size << endl;
						return 1;
					}
					catch(const mysqlpp::Exception & er) {
						cerr << endl << "Error: " <<
						    er.what() << endl;
						return 1;
					}
				}

				dnsperf_stats((char *)domain);
				ldns_rr_list_deep_free(query_results);
				ldns_resolver_deep_free(actual_res);
			}
			sleep(dnsperf_freq);
		}

		if (dnsperf_dumpdb) {
			mysqlpp::Query query_queries = conn.query();
			query_queries << "select * from %6:table";
			query_queries.parse();
			query_queries.template_defaults["table"] =
			    dnsperf_valtable;
			if (dnsperf_dumpdb)
				cout << query_queries << endl;
			mysqlpp::StoreQueryResult res_queries =
			    query_queries.store();

			if (res_queries) {
				cout.setf(ios::left);
				cout << setw(10) << "Domain"
				    << setw(10) << "Latency (us) "
				    << setw(18) << "Date"
				    << setw(18) << "Nameserver" << endl << endl;

				for (size_t i = 0; i < res_queries.num_rows();
				     ++i) {
					cout << setw(30) <<
					    res_queries[i]["domain"] << ' ' <<
					    setw(9) << res_queries[i]["latency"]
					    << ' ' << setw(18) <<
					    res_queries[i]["timestamp"] <<
					    setw(18) << " " <<
					    res_queries[i]["nameserver"] <<
					    endl;
				}
			} else {
				cerr << "Failed to get " << dnsperf_valtable <<
				    " table: " << query_queries.error() << endl;
				return 1;
			}
		}

		return 0;
	} else {
		cerr << "DB connection failed: " << conn.error() << endl;
		return 1;
	}
}

ldns_resolver *build_resolver(const char *domainname,
			      ldns_rr_list ** query_results)
{
	ldns_resolver *res;
	ldns_rdf *domain;
	ldns_pkt *p;
	ldns_status s;
	timeval t1, t2;
	int i = 0;

	p = NULL;
	*query_results = NULL;
	res = NULL;

	domain = ldns_dname_new_frm_str(domainname);
	if (!domain) {
		cout << "failed to build domain to query for NS" << endl;
		exit(EXIT_FAILURE);
	}

	/* create a new resolver from /etc/resolv.conf */
	s = ldns_resolver_new_frm_file(&res, NULL);

	if (s != LDNS_STATUS_OK) {
		cout << "failed to build resolver from file" << endl;
		exit(EXIT_FAILURE);
	}

	p = ldns_resolver_query(res,
				domain,
				LDNS_RR_TYPE_NS, LDNS_RR_CLASS_IN, LDNS_RD);

	ldns_rdf_deep_free(domain);

	if (!p) {
		cout << "failed to query for nameservers" << endl;
		exit(EXIT_FAILURE);
	} else {
		*query_results = ldns_pkt_rr_list_by_type(p,
							  LDNS_RR_TYPE_NS,
							  LDNS_SECTION_ANSWER);
		if (!(*query_results)) {
			printf("Cannot find ns for %s\n", domainname);
		}

	}
	ldns_pkt_free(p);
	return res;
}

unsigned long resolve(const char *domaintoquery, ldns_resolver * actual_res)
{
	ldns_rdf *domaintoq;
	ldns_pkt *p;
	ldns_rr_list *query_results;
	ldns_status s;
	timeval t1, t2;
	unsigned long us1;

	p = NULL;
	query_results = NULL;

	domaintoq = ldns_dname_new_frm_str(domaintoquery);
	if (!domaintoq) {
		cout << "failed to build domain to query" << endl;
		exit(EXIT_FAILURE);
	}

	gettimeofday(&t1, NULL);
	p = ldns_resolver_query(actual_res,
				domaintoq,
				LDNS_RR_TYPE_A, LDNS_RR_CLASS_IN, LDNS_RD);
	gettimeofday(&t2, NULL);

	ldns_rdf_deep_free(domaintoq);

	if (!p) {
		cout << "failed to query for the domain" << endl;
		//exit(EXIT_FAILURE);
		return 0;
	} else {
		query_results = ldns_pkt_rr_list_by_type(p,
							 LDNS_RR_TYPE_A,
							 LDNS_SECTION_ANSWER);
#if 0
		if (!query_results) {
			printf("Cannot resolve %s\n", domaintoquery);
		}
#endif
		us1 = t2.tv_sec * 1000000 + (t2.tv_usec) -
		    (t1.tv_sec * 1000000 + (t1.tv_usec));
//              printf("%lu us elapsed\n", s1);

	}
	ldns_pkt_free(p);
	ldns_rr_list_deep_free(query_results);

	return us1;
}

int dnsperf_stats(char *domain)
{

	/* Connect to the database */
	mysqlpp::Connection conn((bool) false);
	if (dnsperf_dumpdb)
		cout << "Connecting to MYSQL://" << dnsperf_dbuser << "@" <<
		    dnsperf_dbhostname << "/" << dnsperf_dbname << endl;
	if (conn.connect(dnsperf_dbname, dnsperf_dbhostname, dnsperf_dbuser,
			 dnsperf_dbpass)) {
		mysqlpp::Query query = conn.query();
		query << "select " <<
		    "domain," <<
		    "avg(latency)," <<
		    "stddev(latency)," <<
		    "count(latency)," <<
		    "min(timestamp)," <<
		    "max(timestamp) " <<
		    " from " << dnsperf_valtable <<
		    " where domain='" << domain << "';";

		if (dnsperf_dumpdb)
			cout << query << endl;
		mysqlpp::StoreQueryResult res = query.store();

		if (res) {
		/* We don't need labels ;-) */
#if 0
			cout.setf(ios::left);
			cout << setw(10) << "domain" << " "
			    << setw(10) << "avg(latency)" << " "
			    << setw(10) << "stddev(latency)" << " "
			    << setw(10) << "count(latency)" << " "
			    << setw(15) << "min(timestamp)" << " "
			    << setw(15) << "max(timestamp)" << endl << endl;
#endif

			for (size_t i = 0; i < res.num_rows(); ++i) {
				cout.setf(ios::left);
				cout << setw(15) <<
				    res[i]["domain"] << ' ' <<
				    (double)res[i]["avg(latency)"] /
				    1000 << ' ' << setw(10) << (double)
				    res[i]["stddev(latency)"] /
				    1000 << ' ' << setw(10) <<
				    res[i]["count(latency)"] << ' ' << setw(10)
				    << res[i]["min(timestamp)"] << ' ' <<
				    setw(15) << res[i]["max(timestamp)"] << ' '
				    << setw(15) << endl;
			}
		} else {
			cerr << "Failed to get " << dnsperf_valtable <<
			    " table: " << query.error() << endl;
			return 1;
		}
	}

}

int dnsperf_initdb(void)
{
	mysqlpp::Connection con;
	try {
		con.connect(0, "localhost", "root", "");
	}
	catch(exception & er) {
		cerr << "Connection failed: " << er.what() << endl;
		return 1;
	}

	bool new_db = false;
	{
		mysqlpp::NoExceptions ne(con);
		mysqlpp::Query query = con.query();
		if (con.select_db(dnsperf_dbname)) {
			cout << "Dropping existing sample data tables..." <<
			    endl;
			query << "drop table " << dnsperf_valtable;
			query.exec();
			query << "drop table " << dnsperf_domaintable;
			query.exec();
		} else {
			// Database doesn't exist yet, so create and select it.
			if (con.create_db(dnsperf_dbname) &&
			    con.select_db(dnsperf_dbname)) {
				new_db = true;
			} else {
				cerr << "Error creating DB: " << con.error() <<
				    endl;
				return 1;
			}
		}
	}

	try {
		cout << "Creating " << dnsperf_valtable << " table..." << endl;
		mysqlpp::Query query = con.query();
		query <<
		    "CREATE TABLE " << dnsperf_valtable << " (" <<
		    "  domain CHAR(80) NOT NULL, " <<
		    "  latency BIGINT NOT NULL, " <<
		    "  timestamp DATETIME NOT NULL, " <<
		    "  nameserver CHAR(80) NOT NULL, " <<
		    "  notes MEDIUMTEXT NULL) " <<
		    "ENGINE = InnoDB " <<
		    "CHARACTER SET utf8 COLLATE utf8_general_ci";
		query.execute();

		cout << "Creating " << dnsperf_domaintable << " table..." <<
		    endl;
		query << "CREATE TABLE " << dnsperf_domaintable << " (" <<
		    "  rank INT NOT NULL, " << "  domain CHAR(80) NOT NULL, " <<
		    "  notes MEDIUMTEXT NULL) " << "ENGINE = InnoDB " <<
		    "CHARACTER SET utf8 COLLATE utf8_general_ci";
		query.execute();

		if (dnsperf_dumpdb)
			cout << "Populating the " << dnsperf_domaintable <<
			    " table ..." << endl;
		query << "insert into %6:table values " <<
		    "(%0q, %1q, %4q:desc)";
		query.parse();

		query.template_defaults["table"] = dnsperf_domaintable;
		query.template_defaults["notes"] = mysqlpp::null;

		query.execute(1, "google.com");
		query.execute(2, "facebook.com");
		query.execute(3, "youtube.com");
		query.execute(4, "yahoo.com");
		query.execute(5, "live.com");
		query.execute(6, "wikipedia.org");
		query.execute(7, "baidu.com");
		query.execute(8, "blogger.com");
		query.execute(9, "msn.com");
		query.execute(10, "qq.com");

		if (dnsperf_dumpdb) {
			// Test that above did what we wanted.
			cout << "Init complete, query rows..." << endl;
			cout << "inserted " << con.count_rows(dnsperf_valtable)
			    << " rows." << endl;
			cout << "inserted " <<
			    con.count_rows(dnsperf_domaintable) << " rows." <<
			    endl;
		}
		// Report success
		cout << (new_db ? "Created" : "Reinitialized") <<
		    " database successfully." << endl;
	}
	catch(const mysqlpp::BadQuery & er) {
		// Handle any query errors
		cerr << endl << "Query error: " << er.what() << endl;
		return 1;
	}
	catch(const mysqlpp::BadConversion & er) {
		// Handle bad conversions
		cerr << endl << "Conversion error: " << er.what() << endl <<
		    "\tretrieved data size: " << er.retrieved <<
		    ", actual size: " << er.actual_size << endl;
		return 1;
	}
	catch(const mysqlpp::Exception & er) {
		// Catch-all for any other MySQL++ exceptions
		cerr << endl << "Error: " << er.what() << endl;
		return 1;
	}

	return 0;
}
