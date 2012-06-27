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

#define VERSION "0x0"

using namespace std;

int parse_cmdline(int argc, char **argv);
void dnsperf_usage(const char * progname);
void dnsperf_version(void);
int dnsperf_sanity_check(void);

unsigned long resolve(const char *domaintoquery, ldns_resolver * actual_res, char * date);
ldns_resolver *build_resolver(const char *domainname,
			      ldns_rr_list ** query_results);

int dnsperf_stats(char *domain);
int dnsperf_initdb(void);
int dnsperf_create_stattable(mysqlpp::Connection *conn, const char *tablename);
int dnsperf_create_domtable(mysqlpp::Connection *conn, const char *tablename);
int dnsperf_create_valtable(mysqlpp::Connection *conn, const char *tablename);
int dnsperf_update_valtable(mysqlpp::Connection * conn, const char *domain,
			    const char *nameserver, unsigned long timevalue, const char *date);
int dnsperf_get_domains(mysqlpp::Connection * conn,
			mysqlpp::StoreQueryResult * res_domains);

int dnsperf_check_table(mysqlpp::Connection * conn, const char *tablename,
			mysqlpp::StoreQueryResult * res);

#define HOSTNAME_MAX 50
/* cmdline options */
char dnsperf_hostname[HOSTNAME_MAX];
uint8_t dnsperf_resetdb = 0;
uint8_t dnsperf_randhost_idx = 0;
uint8_t dnsperf_verbose = 0;
uint8_t dnsperf_quiet = 0;
unsigned long dnsperf_freq = 1;

/* default database info */
const char *dnsperf_dbhostname = "localhost";
const char *dnsperf_dbname = "dnsperf_data";
const char *dnsperf_dbuser = "root";
const char *dnsperf_dbpass = "";
const char *dnsperf_valtable = "dnsperf_queries";
const char *dnsperf_domaintable = "dnsperf_domains";
const char *dnsperf_stattable = "dnsperf_stats";

#define DNSPERF_DOMAINS 10
const char *default_domains[] = {
	"google.com",
	"facebook.com",
	"youtube.com",
	"yahoo.com",
	"live.com",
	"wikipedia.org",
	"baidu.com",
	"blogger.com",
	"msn.com",
	"qq.com"
};

/* Make sure all database tables exist */
int dnsperf_sanity_check(void)
{
	mysqlpp::Connection conn((bool) false);
	mysqlpp::StoreQueryResult res;
	if (!dnsperf_quiet)
		cout << "Connecting to MYSQL://" << dnsperf_dbuser << "@" <<
		    dnsperf_dbhostname << endl;
	if (!conn.connect(0, dnsperf_dbhostname, dnsperf_dbuser,
			  dnsperf_dbpass)) {
		cerr << "DB connection failed: " << conn.error() << endl;
		return 1;
	}
	if (!dnsperf_quiet)
		cout << "Connected, DBMS active." << endl;

	if (dnsperf_resetdb) {
		/* Reset DB values */
		if (dnsperf_initdb()) {
			cout << "Database init failed, exiting" << endl;
			exit(1);
		}
	} else {
		if (!dnsperf_quiet)
			cout << "Selecting database: `" << dnsperf_dbname << "`" << endl;

		if (!conn.select_db(dnsperf_dbname)) {
			cout << "Database " << dnsperf_dbname <<
			    " does not exist, creating it... " << endl;
			dnsperf_initdb();
		}
	}
	if (conn.select_db(dnsperf_dbname)) {
		/* What happens if the table exists but the schema is different ? ;P */
		if (dnsperf_check_table(&conn, dnsperf_valtable, &res)) {
			if (dnsperf_create_valtable(&conn, dnsperf_valtable)) {
				cout << "Unable to create table `" << dnsperf_valtable << endl;
				return 1;
			}
		}
		if (dnsperf_check_table(&conn, dnsperf_domaintable, &res)) {
			if (dnsperf_create_domtable(&conn, dnsperf_domaintable)) {
				cout << "Unable to create table `" << dnsperf_domaintable << endl;
				return 1;
			}
		}
		if (dnsperf_check_table(&conn, dnsperf_stattable, &res)) {
			if (dnsperf_create_stattable(&conn, dnsperf_stattable)) {
				cout << "Unable to create table `" << dnsperf_stattable<< endl;
				return 1;
			}
		}
	}
	return 0;

}

/* Make sure a specific database table exists
 * FIXME: have to check the table's schema too. */
int dnsperf_check_table(mysqlpp::Connection *conn, const char *tablename, mysqlpp::StoreQueryResult *res)
{
	mysqlpp::Query query = conn->query();

	if (!dnsperf_quiet)
		cout << "Checking table:`" << tablename <<
		    "`" << endl;
	query<< "select * from %6:table";
	query.parse();
	query.template_defaults["table"] = tablename;
	if (dnsperf_verbose)
		cout << query << endl;
	if (!(*res = query.store())) {
		cerr << "Failed to access table `" <<
		    tablename << "` " << query.error() << endl;
		return 1;
	}
	return 0;
}

/* Loop around all domains, query and populate the query log and the stats table */
int dnsperf_do(mysqlpp::Connection * conn,
	       mysqlpp::StoreQueryResult * res_domains)
{

	for (size_t i = 0; i < res_domains->num_rows(); i++) {
		char *nameserver;
		ldns_rdf *ns_name;
		ldns_rr_list *iplist;
		ldns_resolver *actual_res;
		unsigned long timevalue;
		ldns_rr_list *query_results;
		const char *domain = (*res_domains)[i]["domain"].c_str();
		unsigned long nr_nameservers;
		char date[20];


		actual_res = build_resolver(domain, &query_results);

		nr_nameservers = ldns_rr_list_rr_count(query_results);

		for (size_t j = 0; j < nr_nameservers; j++) {
			ldns_resolver *resolver;
			resolver = ldns_resolver_new();
			ns_name =
			    ldns_rr_rdf(ldns_rr_list_rr(query_results, j), 0);
			iplist =
			    ldns_get_rr_list_addr_by_name
			    (actual_res, ns_name, LDNS_RR_CLASS_IN, 0);
			(void)
			    ldns_resolver_push_nameserver_rr_list(resolver,
								  iplist);
			if (!ns_name) {
				cout << "NS name is null\n"<< endl;
				return 1;
			} else {
				nameserver = ldns_rdf2str(ns_name);
			}
			if (dnsperf_verbose)
				cout << "Building random domain to query." <<
				    endl;

			/* construct a random hostname (keeping the relevant domain name) */
			sprintf(dnsperf_hostname, "foo%d", rand() % 1024);
			dnsperf_randhost_idx = strlen(dnsperf_hostname);
			if (dnsperf_verbose)
				cout << "Relative domain to query is `" <<
				    dnsperf_hostname << "`" << endl;

			sprintf(dnsperf_hostname + dnsperf_randhost_idx, ".%s",
				domain);

			/* do the actual query and measure time */
			timevalue = resolve(dnsperf_hostname, resolver, date);
			if (!timevalue)
				/* If we fail, then too bad for the domain :-) */
				continue;


			ldns_rr_list_free(iplist);
			ldns_resolver_deep_free(resolver);

			/* perform the SQL query to update the table that holds query logs */
			dnsperf_update_valtable(conn, domain, nameserver,
						timevalue, date);
			free(nameserver);
		}

		dnsperf_stats((char *)domain);
		ldns_rr_list_deep_free(query_results);
		ldns_resolver_deep_free(actual_res);
	}
	return 0;
}

/* Get the domains to test from the database table. Populate the MySQL result structure */
int dnsperf_get_domains(mysqlpp::Connection * conn,
			mysqlpp::StoreQueryResult * res_domains)
{
	mysqlpp::Query query_domains = conn->query();
	if (!dnsperf_quiet)
		cout << "Database selected." << endl;

	if (!dnsperf_quiet)
		cout << "Getting domains from table `" << dnsperf_domaintable <<
		    "`" << endl;
	query_domains << "select * from %6:table";
	query_domains.parse();
	query_domains.template_defaults["table"] = dnsperf_domaintable;
	if (dnsperf_verbose)
		cout << query_domains << endl;
	*res_domains = query_domains.store();

	if (!(*res_domains)) {
		cerr << "Failed to get domains from `" <<
		    dnsperf_domaintable << "` " << query_domains.error() <<
		    endl;
		return 1;
	}
	if (dnsperf_verbose) {
		cout.setf(ios::left);
		cout <<
		    setw(10) << "Rank" << setw(10) << "Domain" << endl << endl;

		for (size_t i = 0; i < res_domains->num_rows(); ++i) {
			cout << setw(5) <<
			    (*res_domains)[i]["rank"] << ' ' <<
			    setw(30) << (*res_domains)[i]["domain"]
			    << ' ' << endl;
		}
	}
	return 0;
}

int main(int argc, char *argv[])
{
	/* Init rand() */
	srand((unsigned)time(NULL));

	if (parse_cmdline(argc, argv)) {
		exit(1);
	}

#if 0
	if (dnsperf_resetdb) {
		/* Reset DB values */
		if (dnsperf_initdb()) {
			cout << "Database init failed, exiting" << endl;
			exit(1);
		}
	}
#endif

	/* Connect to the database */
	mysqlpp::Connection conn((bool) false);
	if (!dnsperf_quiet)
		cout << "Connecting to MYSQL://" << dnsperf_dbuser << "@" <<
		    dnsperf_dbhostname << endl;
	if (!conn.connect(0, dnsperf_dbhostname, dnsperf_dbuser,
			  dnsperf_dbpass)) {
		cerr << "DB connection failed: " << conn.error() << endl;
		return 1;
	}
	if (!dnsperf_quiet)
		cout << "Connected." << endl;

	if (!dnsperf_quiet)
		cout << "Selecting database: `" << dnsperf_dbname << "`" << endl;
	if (!conn.select_db(dnsperf_dbname)) {
		cout << "Database " << dnsperf_dbname <<
		    " does not exist, creating it... " << endl;
		if (dnsperf_initdb()) {
			cout << "Unable to initialize database" << endl;
			return 1;
		}
	}

	if (conn.select_db(dnsperf_dbname)) {
		int iter = 0;
		mysqlpp::StoreQueryResult res_domains;

		/* Domains are stored on the mysql result structure */
		if (dnsperf_get_domains(&conn, &res_domains)) {
			cout << "Unable to get domains" << endl;
			return 1;
		}
		cout << "Starting to loop..." << endl;
		while (1) {
			/* We pass the result structure to the function
			 * that does the actual work */
			if (dnsperf_do(&conn, &res_domains)) {
				cout << "Failed" << endl;
				return 1;
			}
#if 0
			if (dnsperf_verbose) {
				dnsperf_dump_valtable(&conn);
			}
#endif
			cout << "Iteration " << ++iter << " done, sleeping for "
			    << dnsperf_freq << "ms. " << endl;
			usleep(dnsperf_freq * 1000);
		}
	}

	return 0;
}

/* Get all namservers for a domain, and populate an LDNS list */
ldns_resolver *build_resolver(const char *domainname,
			      ldns_rr_list ** query_results)
{
	ldns_resolver *res;
	ldns_rdf *domain;
	ldns_pkt *p;
	ldns_status s;

	p = NULL;
	*query_results = NULL;
	res = NULL;

	domain = ldns_dname_new_frm_str(domainname);
	if (!domain) {
		cout << "failed to build domain to query for NS" << endl;
		exit(EXIT_FAILURE);
	}

	/* create a new resolver from /etc/resolv.conf
	 * to get the domain's nameservers */
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

/* Do the actual query, using a resolver */
unsigned long resolve(const char *domaintoquery, ldns_resolver * actual_res, char *date)
{
	ldns_rdf *domaintoq;
	ldns_pkt *p;
	ldns_rr_list *query_results;
	timeval t1, t2;
	unsigned long us1;
	time_t tm;
	struct tm *tm_local;

	p = NULL;
	query_results = NULL;

	domaintoq = ldns_dname_new_frm_str(domaintoquery);
	if (!domaintoq) {
		cout << "failed to build domain to query" << endl;
		exit(EXIT_FAILURE);
	}

	tm = time(NULL);
	gettimeofday(&t1, NULL);
	p = ldns_resolver_query(actual_res,
				domaintoq,
				LDNS_RR_TYPE_A, LDNS_RR_CLASS_IN, LDNS_RD);
	gettimeofday(&t2, NULL);

	/* build the timestamp in a MySQL format */
	tm_local = localtime(&tm);
	strftime(date, 20, "%Y-%m-%d %X", tm_local);


	ldns_rdf_deep_free(domaintoq);

	if (!p) {
		cout << "failed to query for `" << domaintoquery << "`" << endl;
		/* No need to fail, we just got a timeout or something */
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

/* Populate query logs to the database */
int dnsperf_update_valtable(mysqlpp::Connection * conn, const char* domain, const char*nameserver, unsigned long timevalue, const char *date)
{
	try {
		mysqlpp::Query query = conn->query();
		if (dnsperf_verbose)
			cout <<
			    "Updating mysql table "
			    << dnsperf_valtable << " ..." << endl;

		query <<
		    "insert into %6:table values "
		    << "(%0q, %1q, %2q, %3q) ";
		query.parse();

		query.template_defaults["table"]
		    = dnsperf_valtable;

		if (dnsperf_verbose)
			cout << "Populating " <<
			    dnsperf_valtable << " table..." << flush;
		query.execute(domain, timevalue, date, nameserver);

		if (dnsperf_verbose) {
			cout << "inserted " << 1 << " row." << endl;
			cout << dnsperf_hostname
			    << " " << domain <<
			    " " << timevalue <<
			    " " << date << " " << nameserver << endl;
		}
	}
	catch(const mysqlpp::BadQuery & er) {
		cerr << endl << "Query error: " << er.what() << endl;
		return 1;
	}
	catch(const mysqlpp::BadConversion & er) {
		cerr << endl <<
		    "Conversion error: " <<
		    er.what() << endl <<
		    "\tretrieved data size: " <<
		    er.retrieved << ", actual size: " << er.actual_size << endl;
		return 1;
	}
	catch(const mysqlpp::Exception & er) {
		cerr << endl << "Error: " << er.what() << endl;
		return 1;
	}

	return 0;
}

/* Generate stats (using MySQL macros) and populate the stats table */
int dnsperf_stats(char *domain)
{

	/* Connect to the database */
	mysqlpp::Connection conn((bool) false);
	if (dnsperf_verbose)
		cout << "Connecting to MYSQL://" << dnsperf_dbuser << "@" <<
		    dnsperf_dbhostname << "/" << dnsperf_dbname << endl;
	if (conn.connect(dnsperf_dbname, dnsperf_dbhostname, dnsperf_dbuser,
			 dnsperf_dbpass)) {

		/* Check that the domain has answered at least one of our queries */
		mysqlpp::Query query_check = conn.query();
		query_check << "select *" <<
		    " from " << dnsperf_valtable <<
		    " where domain='" << domain << "';";
		mysqlpp::StoreQueryResult res_check = query_check.store();
		if (!res_check.num_rows()){
			return 1;
		}

		/* Use mysql to calculate avg and stddev */
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

		if (dnsperf_verbose)
			cout << query << endl;
		mysqlpp::StoreQueryResult res = query.store();

		if (res) {
			double avg;
			double stddev;
			uint32_t count;
			char timestamp_first[20], timestamp_last[20];

			avg = res[0]["avg(latency)"];
			stddev = res[0]["stddev(latency)"];
			count = res[0]["count(latency)"];
			sprintf(timestamp_first, res[0]["min(timestamp)"]);
			sprintf(timestamp_last, res[0]["max(timestamp)"]);
			if (!dnsperf_quiet) {
				cout << "domain: " << domain << " "
				    << "count: " << count << " queries, "
				    << "Avg: " << avg / 1000.0 << " ms, "
				    << "Stddev: " << stddev / 1000.0 << " ms, "
				    << "first query: " << timestamp_first <<
				    ", " << "last query: " << timestamp_last <<
				    endl;
			}

			query = conn.query();
			query << "update " <<
			    dnsperf_stattable <<
			    " set average = " << avg <<
			    ", stddev = " << stddev <<
			    ", count = " << count <<
			    ", first = '" << timestamp_first <<
			    "', last = '" << timestamp_last <<
			    "' where domain='" << domain << "';";

			if (dnsperf_verbose)
				cout << query << endl;
			mysqlpp::StoreQueryResult res_stats = query.store();
			if (res_stats) {
				cerr << "Failed to update " << dnsperf_stattable
				    << " table: " << query.error() << endl;
				/* FIXME: how critical is this ? Should we fail ? */
				return 1;
			}
		} else {
			cerr << "Failed to get " << dnsperf_valtable <<
			    " table: " << query.error() << endl;
			return 1;
		}
	}

	return 0;
}

/* Database init functions */
int dnsperf_create_stattable(mysqlpp::Connection *conn, const char *tablename)
{

	try {
		if (!dnsperf_quiet)
			cout << "Creating " << tablename << " table..." << endl;
		mysqlpp::Query query = conn->query();
		query<<
		    "CREATE TABLE " << tablename << " (" <<
		    "  domain CHAR(80) NOT NULL, " <<
		    "  average DOUBLE NULL, " <<
		    "  stddev DOUBLE NULL, " <<
		    "  count BIGINT NULL, " <<
		    "  first DATETIME NULL, " <<
		    "  last DATETIME NULL ) " <<
		    "ENGINE = InnoDB " <<
		    "CHARACTER SET utf8 COLLATE utf8_general_ci";
		query.execute();

		query<< "insert into %6:table values " <<
		    "(%0q, %1q, %2q, %3q, %4q, %5q)";
		query.parse();

		query.template_defaults["table"] = tablename;

		for (size_t i = 0; i < DNSPERF_DOMAINS; ++i)
			query.execute(default_domains[i], 0, 0, 0, 0, 0);
	}
	catch(const mysqlpp::BadQuery & er) {
		cerr << endl << "Query error: " << er.what() << endl;
		return 1;
	}
	catch(const mysqlpp::BadConversion & er) {
		cerr << endl << "Conversion error: " << er.what() << endl <<
		    "\tretrieved data size: " << er.retrieved <<
		    ", actual size: " << er.actual_size << endl;
		return 1;
	}
	catch(const mysqlpp::Exception & er) {
		cerr << endl << "Error: " << er.what() << endl;
		return 1;
	}
	return 0;
}

int dnsperf_create_domtable(mysqlpp::Connection * conn, const char *tablename)
{
	try {
		mysqlpp::Query query = conn->query();

		if (!dnsperf_quiet)
			cout << "Creating " << tablename << " table..." << endl;
		query << "CREATE TABLE " << tablename << " (" <<
		    "  rank INT NOT NULL, " <<
		    "  domain CHAR(80) NOT NULL) " <<
		    "ENGINE = InnoDB " <<
		    "CHARACTER SET utf8 COLLATE utf8_general_ci";
		query.execute();

		if (dnsperf_verbose)
			cout << "Populating the " << tablename <<
			    " table ..." << endl;
		query << "insert into %6:table values " <<
		    "(%0q, %1q )";
		query.parse();

		query.template_defaults["table"] = tablename;

		for (size_t i = 1; i <= DNSPERF_DOMAINS; ++i)
			query.execute(i, default_domains[i - 1]);
	}
	catch(const mysqlpp::BadQuery & er) {
		cerr << endl << "Query error: " << er.what() << endl;
		return 1;
	}
	catch(const mysqlpp::BadConversion & er) {
		cerr << endl << "Conversion error: " << er.what() << endl <<
		    "\tretrieved data size: " << er.retrieved <<
		    ", actual size: " << er.actual_size << endl;
		return 1;
	}
	catch(const mysqlpp::Exception & er) {
		cerr << endl << "Error: " << er.what() << endl;
		return 1;
	}
	return 0;
}

int dnsperf_create_valtable(mysqlpp::Connection *conn, const char *tablename)
{
	try {
		if (!dnsperf_quiet)
			cout << "Creating " << tablename << " table..." << endl;
		mysqlpp::Query query = conn->query();
		query <<
		    "CREATE TABLE " << tablename << " (" <<
		    "  domain CHAR(80) NOT NULL, " <<
		    "  latency BIGINT NOT NULL, " <<
		    "  timestamp DATETIME NOT NULL, " <<
		    "  nameserver CHAR(80) NOT NULL) " <<
		    "ENGINE = InnoDB " <<
		    "CHARACTER SET utf8 COLLATE utf8_general_ci";
		query.execute();

	}
	catch(const mysqlpp::BadQuery & er) {
		cerr << endl << "Query error: " << er.what() << endl;
		return 1;
	}
	catch(const mysqlpp::BadConversion & er) {
		cerr << endl << "Conversion error: " << er.what() << endl <<
		    "\tretrieved data size: " << er.retrieved <<
		    ", actual size: " << er.actual_size << endl;
		return 1;
	}
	catch(const mysqlpp::Exception & er) {
		cerr << endl << "Error: " << er.what() << endl;
		return 1;
	}
	return 0;

}

int dnsperf_initdb(void)
{
	mysqlpp::Connection con;
	try {
		con.connect(0, dnsperf_dbhostname, dnsperf_dbuser,
			    dnsperf_dbpass);
	}
	catch(exception & er) {
		cerr << "Connection failed: " << er.what() << endl;
		return 1;
	}

	bool new_db = false;

	mysqlpp::NoExceptions ne(con);
	mysqlpp::Query query = con.query();

	if (con.select_db(dnsperf_dbname)) {
		cout << "Dropping existing tables..." << endl;
		query << "drop table " << dnsperf_valtable;
		query.exec();
		query << "drop table " << dnsperf_domaintable;
		query.exec();
		query << "drop table " << dnsperf_stattable;
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

	cout << (new_db ? "Created" : "Reinitialized") <<
		    " database successfully." << endl;

	if (dnsperf_create_valtable(&con, dnsperf_valtable)) {
		cout << "Unable to create table `" << dnsperf_valtable << endl;
		exit(1);
	}
	if (dnsperf_create_domtable(&con, dnsperf_domaintable)) {
		cout << "Unable to create table `" << dnsperf_domaintable << endl;
		exit(1);
	}
	if (dnsperf_create_stattable(&con, dnsperf_stattable)) {
		cout << "Unable to create table `" << dnsperf_stattable<< endl;
		exit(1);
	}

	return 0;
}

/* Various helper functions */
int parse_cmdline(int argc, char **argv)
{
	int c;

	opterr = 0;

	while ((c = getopt(argc, argv, "qVhvru:p:m:c:t:d:s:f:")) != -1)
		switch (c) {
		case 'q':
			dnsperf_quiet = 1;
			break;
		case 'V':
			dnsperf_version();
			break;
		case 'f':
			dnsperf_freq = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			dnsperf_verbose = 1;
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
		case 's':
			dnsperf_stattable = strdup(optarg);
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
		case 'h':
			dnsperf_usage(argv[0]);
			break;
		case '?':
			cout << "Error parsing arguments" << endl;
			dnsperf_usage(argv[0]);
			return 1;
		default:
			abort();
		}

	return dnsperf_sanity_check();
}

void dnsperf_version(void)
{
	printf("Version %s\n", VERSION);
	exit(0);
}
void dnsperf_usage(const char * progname)
{
	printf("%s <options> \n", progname);
	printf("options: [-h] | [-V] | [-v] [-f <ms>] [-r] [-u <dbuser>] [-p <dbpass] \n"
	       "         [-c <dbhostname>] [-m <dbname>] [-t <logtable>] [-d <domaintable>] [-s <stattable>]\n\n");

	printf("  -h			  print this help and exit\n");
	printf("  -V			  print version and exit\n\n");

	printf("  -q			  supress stat output\n");
	printf("  -v			  verbose output\n");
	printf("  -f <time>		  time to wait after each loop (in ms)\n\n");

	printf("  Database Specific (MySQL)\n");
	printf("  -r			  re-initialize database (WARNING: all existing data will be lost)\n");
	printf("  -u <user>		  user to connect to database (default: root)\n");
	printf("  -p <pass>		  pass to connect to database (default: <empty>)\n");
	printf("  -c <hostname>		  hostname that MySQL is running (default: localhost)\n");
	printf("  -m <dbname>		  MySQL database name (default: dnsperf_data)\n"
	       "                            if it doesn't exist, we create it, implies -r\n");
	printf("  -t <table>		  table name for logging queries (default: dnsperf_queries)\n");
	printf("  -d <table>		  table name for top level domains (default: dnsperf_domains)\n");
	printf("  -s <table>		  table name for stats (default: dnsperf_stattable)\n\n");

	exit(0);
}

int dnsperf_dump_valtable(mysqlpp::Connection *conn)
{

	mysqlpp::Query query_queries = conn->query();
	query_queries << "select * from %6:table";
	query_queries.parse();
	query_queries.template_defaults["table"] =
	    dnsperf_valtable;
	if (dnsperf_verbose)
		cout << query_queries << endl;
	mysqlpp::StoreQueryResult res_queries =
	    query_queries.store();
	if (!res_queries) {
		cerr << "Failed to get `" << dnsperf_valtable <<
		    "`: " << query_queries.error() << endl;
		return 1;
	}
	cout.setf(ios::left);
	cout << setw(10) << "Domain"
	    << setw(10) << "Latency (us) "
	    << setw(18) << "Date"
	    << setw(18) << "Nameserver" << endl << endl;

	for (size_t i = 0; i < res_queries.num_rows(); ++i) {
		cout << setw(30) <<
		    res_queries[i]["domain"] << ' ' <<
		    setw(9) << res_queries[i]["latency"]
		    << ' ' << setw(18) <<
		    res_queries[i]["timestamp"] <<
		    setw(18) << " " <<
		    res_queries[i]["nameserver"] << endl;
	}
	return 0;
}
