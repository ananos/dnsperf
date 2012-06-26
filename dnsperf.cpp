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

char * dnsperf_hostname = NULL;
uint8_t dnsperf_resetdb = 0;
int dnsperf_initdb(void);
unsigned long resolve(const char *domainname, const char *domaintoquery);

int parse_cmdline(int argc, char **argv)
{
	int aflag = 0;
	int bflag = 0;
	char *cvalue = NULL;
	int index;
	int c;

	opterr = 0;

	while ((c = getopt(argc, argv, "rh:")) != -1)
		switch (c) {
		case 'r':
			dnsperf_resetdb = 1;
			break;
		case 'h':
			dnsperf_hostname = strdup(optarg);
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
#if 0
	// Get database access parameters from command line
	mysqlpp::examples::CommandLine cmdline(argc, argv);
	if (!cmdline) {
		return 1;
	}
#endif
	if (parse_cmdline(argc, argv)) {
		cout << "Error parsing arguments" << endl;
		exit(1);
	}
	if (dnsperf_resetdb) {
		if (dnsperf_initdb()) {
			cout << "Database init failed, exiting" << endl;
			exit(1);
		}
	}
	// Connect to the sample database.
	mysqlpp::Connection conn((bool) false);
	if (conn.connect("mysql_cpp_data", "localhost", "root", "")) {
		int i = 0;

		// Retrieve the sample stock table set up by resetdb
		mysqlpp::Query query_domains =
		    conn.query("select * from domains");
		mysqlpp::StoreQueryResult res_domains = query_domains.store();

		// Display results
		if (res_domains) {
			// Display header
			cout.setf(ios::left);
			cout <<
			    setw(10) << "Rank" <<
			    setw(10) << "Domain" << endl << endl;

			// Get each row in result set, and print its contents
			for (size_t i = 0; i < res_domains.num_rows(); ++i) {
				cout << setw(5) << res_domains[i]["rank"] << ' '
				    << setw(30) << res_domains[i]["domain"] <<
				    ' ' << endl;
			}
		} else {
			cerr << "Failed to get dnsqueries table: " <<
			    query_domains.error() << endl;
			return 1;
		}

		for (i = 0; i < res_domains.num_rows(); i++) {

			timevalue =
			    resolve(res_domains[i]["domain"].c_str(), dnsperf_hostname);
			//gettimeofday(&currenttime,NULL);

			try {
				mysqlpp::Query query = conn.query();
				cout << "Updating mysql table dnsqueries..." <<
				    endl;

				query << "insert into %6:table values " <<
				    "(%0q, %1q, %2q, %3q:desc)";
				query.parse();

				query.template_defaults["table"] = "dnsqueries";
				query.template_defaults["desc"] = mysqlpp::null;

				cout << "Populating dnsqueries table..." <<
				    flush;
				char date[20];
				time_t tm;
				struct tm *tmp;
				tm = time(NULL);
				tmp = localtime(&tm);
				strftime(date, 20, "%Y-%m-%d %X", tmp);
				query.execute(res_domains[i]["domain"].c_str(),
					      timevalue, date);

				cout << "inserted " <<
				    conn.count_rows("dnsqueries") << " rows." <<
				    endl;
			}
			catch(const mysqlpp::BadQuery & er) {
				cerr << endl << "Query error: " << er.what() <<
				    endl;
				return 1;
			}
			catch(const mysqlpp::BadConversion & er) {
				cerr << endl << "Conversion error: " <<
				    er.what() << endl <<
				    "\tretrieved data size: " << er.retrieved <<
				    ", actual size: " << er.actual_size << endl;
				return 1;
			}
			catch(const mysqlpp::Exception & er) {
				cerr << endl << "Error: " << er.what() << endl;
				return 1;
			}
		}

		mysqlpp::Query query_queries =
		    conn.query("select * from dnsqueries");
		mysqlpp::StoreQueryResult res_queries = query_queries.store();

		if (res_queries) {
			cout.setf(ios::left);
			cout <<
			    setw(10) << "Domain" <<
			    setw(10) << "Latency" << "Date" << endl << endl;

			for (size_t i = 0; i < res_queries.num_rows(); ++i) {
				cout << setw(30) << res_queries[i]["domain"] <<
				    ' ' << setw(9) << res_queries[i]["latency"]
				    << ' ' << setw(9) <<
				    res_queries[i]["timestamp"] << endl;
			}
		} else {
			cerr << "Failed to get dnsqueries table: " <<
			    query_queries.error() << endl;
			return 1;
		}

		return 0;
	} else {
		cerr << "DB connection failed: " << conn.error() << endl;
		return 1;
	}
}

unsigned long resolve(const char *domainname, const char *domaintoquery)
{
	ldns_resolver *res, *actual_res;
	ldns_rdf *domain, *domaintoq;
	ldns_pkt *p;
	ldns_rdf *ns_name, *ns_ip, **ns_iplist;
	ldns_rr_list *query_results;
	ldns_rr_list *iplist;
	ldns_status s;
	timeval t1, t2;
	unsigned long us1, us2;
	int i;

	p = NULL;
	query_results = NULL;
	res = NULL;

	domain = ldns_dname_new_frm_str(domainname);
	if (!domain) {
		exit(EXIT_FAILURE);
	}

	domaintoq = ldns_dname_new_frm_str(domaintoquery);
	if (!domaintoq) {
		exit(EXIT_FAILURE);
	}

	actual_res = ldns_resolver_new();
	if (!actual_res) {
		exit(EXIT_FAILURE);
	}
	/* create a new resolver from /etc/resolv.conf */
	s = ldns_resolver_new_frm_file(&res, NULL);

	if (s != LDNS_STATUS_OK) {
		exit(EXIT_FAILURE);
	}

	p = ldns_resolver_query(res,
				domain,
				LDNS_RR_TYPE_NS, LDNS_RR_CLASS_IN, LDNS_RD);

	ldns_rdf_deep_free(domain);

	if (!p) {
		exit(EXIT_FAILURE);
	} else {
		query_results = ldns_pkt_rr_list_by_type(p,
							 LDNS_RR_TYPE_NS,
							 LDNS_SECTION_ANSWER);
		if (!query_results) {
			printf("Cannot find ns for %s\n", domainname);
		}

		for (i = 0; i < ldns_rr_list_rr_count(query_results); i++) {
			ns_name =
			    ldns_rr_rdf(ldns_rr_list_rr(query_results, i), 0);
			printf("id = %d\n", i);
			if (!ns_name) {
				printf("error, ns_name is null\n");
			}
			iplist =
			    ldns_get_rr_list_addr_by_name(res, ns_name,
							  LDNS_RR_CLASS_IN, 0);
			(void)ldns_resolver_push_nameserver_rr_list(actual_res,
								    iplist);

		}

		ldns_resolver_nameservers_randomize(actual_res);
		ldns_rr_list_deep_free(iplist);
	}

	ldns_pkt_free(p);
	// ldns_resolver_deep_free(res);

#if 1
	gettimeofday(&t1, NULL);
	p = ldns_resolver_query(actual_res,
				domaintoq,
				LDNS_RR_TYPE_A, LDNS_RR_CLASS_IN, LDNS_RD);
	gettimeofday(&t2, NULL);

	ldns_rdf_deep_free(domaintoq);

	if (!p) {
		exit(EXIT_FAILURE);
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
	ldns_resolver_deep_free(actual_res);
#endif

	return us1;
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
		if (con.select_db("mysql_cpp_data")) {
			cout << "Dropping existing sample data tables..." <<
			    endl;
			query.exec("drop table dnsqueries");
			query.exec("drop table domains");
		} else {
			// Database doesn't exist yet, so create and select it.
			if (con.create_db("mysql_cpp_data") &&
			    con.select_db("mysql_cpp_data")) {
				new_db = true;
			} else {
				cerr << "Error creating DB: " << con.error() <<
				    endl;
				return 1;
			}
		}
	}

	try {
		cout << "Creating dnsqueries table..." << endl;
		mysqlpp::Query query = con.query();
		query <<
		    "CREATE TABLE dnsqueries (" <<
		    "  domain CHAR(80) NOT NULL, " <<
		    "  latency BIGINT NOT NULL, " <<
		    "  timestamp DATETIME NOT NULL, " <<
		    "  notes MEDIUMTEXT NULL) " <<
		    "ENGINE = InnoDB " <<
		    "CHARACTER SET utf8 COLLATE utf8_general_ci";
		query.execute();

		cout << "Creating domains table..." << endl;
		query <<
		    "CREATE TABLE domains (" <<
		    "  rank INT NOT NULL, " <<
		    "  domain CHAR(80) NOT NULL, " <<
		    "  notes MEDIUMTEXT NULL) " <<
		    "ENGINE = InnoDB " <<
		    "CHARACTER SET utf8 COLLATE utf8_general_ci";
		query.execute();

		cout << "Populating the domains table ..." << endl;
		query << "insert into %6:table values " <<
		    "(%0q, %1q, %4q:desc)";
		query.parse();

		query.template_defaults["table"] = "domains";
		query.template_defaults["notes"] = mysqlpp::null;

//              query.execute(1, "nanos.gr");
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

		// Test that above did what we wanted.
		cout << "Init complete, query rows..." << endl;
		cout << "inserted " << con.count_rows("dnsqueries") << " rows."
		    << endl;
		cout << "inserted " << con.count_rows("domains") << " rows." <<
		    endl;

		// Report success
		cout << (new_db ? "Created" : "Reinitialized") <<
		    " sample database successfully." << endl;
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
