// Copyright 2017 NSONE, Inc

#include <algorithm>
#include <iostream>
#include <iterator>
#include <map>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <httplib.h>

#include "config.h"
#include "docopt.h"
#include "http.h"
#include "metrics.h"
#include "query.h"
#include "trafgen.h"
#include "utils.h"

#include <uvw.hpp>

#include "version.h"

#ifdef USE_HTTP_PARSER
#include <http_parser.h>
#else
#include <url_parser.h>
#endif

static const char METRIC_ROUTE[] = "/api/v1/metrics";
static const char USAGE[] =
    R"(Flamethrower.
    Usage:
      flame [-b BIND_IP] [-q QCOUNT] [-c TCOUNT] [-p PORT] [-d DELAY_MS] [-r RECORD] [-T QTYPE]
            [-o FILE] [-l LIMIT_SECS] [-t TIMEOUT] [-F FAMILY] [-f FILE] [-n LOOP] [-P PROTOCOL] [-M HTTPMETHOD]
            [-Q QPS] [-g GENERATOR] [-v VERBOSITY] [-R] [--class CLASS] [--qps-flow SPEC]
            [--dnssec] [--targets FILE] [--http-srv PORT]
            TARGET [GENOPTS]...
      flame (-h | --help)
      flame --version

    TARGET may be a hostname, an IP address, or a comma separated list of either. If multiple targets are specified,
    they will be sent queries in a strict round robin fashion across all concurrent generators. All targets must
    share the same port, protocol, and internet family.

    TARGET may also be the special value "file", in which case the --targets option needs to also be specified.

    Options:
      -h --help        Show this screen
      --version        Show version
      --class CLASS    Default query class, defaults to IN. May also be CH [default: IN]
      -b BIND_IP       IP address to bind to [defaults: 0.0.0.0 for inet, ::0 for inet6]
      -c TCOUNT        Number of concurrent traffic generators per process [default: 10]
      -d DELAY_MS      ms delay between each traffic generator's query [default: 1]
      -q QCOUNT        Number of queries to send every DELAY ms [default: 10]
      -l LIMIT_SECS    Limit traffic generation to N seconds, 0 is unlimited [default: 0]
      -t TIMEOUT_SECS  Query timeout in seconds [default: 3]
      -n LOOP          Loop LOOP times through record list, 0 is unlimited [default: 0]
      -Q QPS           Rate limit to a maximum of QPS, 0 is no limit [default: 0]
      --qps-flow SPEC  Change rate limit over time, format: QPS,MS;QPS,MS;...
      -r RECORD        The base record to use as the DNS query for generators [default: test.com]
      -T QTYPE         The query type to use for generators [default: A]
      -f FILE          Read records from FILE, one per row, QNAME TYPE
      -p PORTS         Which port(s) to flame. Can be a single port, a comma-separated list (e.g., 53,853),
                       or a range (e.g., 53-55). [defaults: 53 for UDP/TCP, 443 for DoH, 853 for DoT]
      -F FAMILY        Internet family (inet/inet6) [default: inet]
      -P PROTOCOL      Protocol to use (udp/tcp/dot/doh) [default: udp]
      -M HTTPMETHOD    HTTP method to use (POST/GET) when DoH is used [default: GET]
      -g GENERATOR     Generate queries with the given generator [default: static]
      -o FILE          Metrics output file, JSON format
      --http-srv PORT  Expose JSON metrics via HTTP on the specified port
      -v VERBOSITY     How verbose output should be, 0 is silent [default: 1]
      -R               Randomize the query list before sending [default: false]
      --targets FILE   Get the list of TARGETs from the given file, one line per host or IP
      --dnssec         Set DO flag in EDNS

     Generators:

       Using generator modules you can craft the type of packet or query which is sent.

       Specify generator arguments by passing in KEY=VAL pairs, where the KEY is a specific configuration
       key interpreted by the generator as specified below in caps (although keys are not case sensitive).

       static                  The basic static generator, used by default, has a single qname/qtype
                               which you can set with -r and -T. There are no KEYs for this generator.

       file                    The basic file generator, used with -f, reads in one qname/qtype pair
                               per line in the file. There are no KEYs for this generator.

       numberqname             Synthesize qnames with random numbers, between [LOW, HIGH], at zone specified with -r

                    LOW        An integer representing the lowest number queried, default 0
                    HIGH       An integer representing the highest number queried, default 100000

       randompkt               Generate COUNT randomly generated packets, of random size [1,SIZE]

                    COUNT      An integer representing the number of packets to generate, default 1000
                    SIZE       An integer representing the maximum size of the random packet, default 600

       randomqname             Generate COUNT queries of randomly generated QNAME's (including nulls) of random length
                               [1,SIZE], at base zone specified with -r

                    COUNT      An integer representing the number of queries to generate, default 1000
                    SIZE       An integer representing the maximum length of the random qname, default 255

       randomlabel             Generate COUNT queries in base zone, each with LBLCOUNT random labels of size [1,LBLSIZE]
                               Use -r to set the base zone to create the labels in. Queries will have a random QTYPE
                               from the most popular set.

                    COUNT      An integer representing the number of queries to generate, default 1000
                    LBLSIZE    An integer representing the maximum length of a single label, default 10
                    LBLCOUNT   An integer representing the maximum number of labels in the qname, default 5


     Generator Example:
        flame target.test.com -T ANY -g randomlabel lblsize=10 lblcount=4 count=1000

)";

void parse_flowspec(std::string spec, std::queue<std::pair<uint64_t, uint64_t>> &result, int verbosity, long c_count)
{

    std::vector<std::string> groups = split(spec, ';');
    for (unsigned i = 0; i < groups.size(); i++) {
        std::vector<std::string> nums = split(groups[i], ',');
        if (verbosity > 1) {
            std::cout << "adding QPS flow: " << nums[0] << "qps, " << nums[1] << "ms" << std::endl;
        }
        long want_r = std::stol(nums[0]);
        if (want_r < c_count) {
            std::cerr << "WARNING: QPS flow limit is less than concurrent senders, changing limit to " << c_count << std::endl;
            want_r = c_count;
        }
        result.push(std::make_pair(want_r, std::stol(nums[1])));
    }
}

std::vector<unsigned int> parse_ports(const std::string &port_spec)
{
    std::vector<unsigned int> ports;
    std::stringstream ss(port_spec);
    std::string segment;

    while (std::getline(ss, segment, ',')) {
        size_t hyphen_pos = segment.find('-');
        if (hyphen_pos == std::string::npos) {
            // Single port
            try {
                long port = std::stol(segment);
                if (port <= 0 || port > 65535) {
                    throw std::runtime_error("Port out of range (1-65535): " + segment);
                }
                ports.push_back(static_cast<unsigned int>(port));
            } catch (const std::exception &e) {
                throw std::runtime_error("Invalid port number: " + segment + " (" + e.what() + ")");
            }
        } else {
            // Port range
            std::string start_str = segment.substr(0, hyphen_pos);
            std::string end_str = segment.substr(hyphen_pos + 1);
            if (start_str.empty() || end_str.empty()) {
                throw std::runtime_error("Invalid port range format: " + segment);
            }
            try {
                long start_port = std::stol(start_str);
                long end_port = std::stol(end_str);

                if (start_port <= 0 || start_port > 65535 || end_port <= 0 || end_port > 65535) {
                    throw std::runtime_error("Port range value out of range (1-65535): " + segment);
                }
                if (start_port > end_port) {
                    throw std::runtime_error("Invalid port range order (start > end): " + segment);
                }
                for (long p = start_port; p <= end_port; ++p) {
                    ports.push_back(static_cast<unsigned int>(p));
                }
            } catch (const std::exception &e) {
                throw std::runtime_error("Invalid port range number: " + segment + " (" + e.what() + ")");
            }
        }
    }

    if (ports.empty()) {
        throw std::runtime_error("No ports specified or parsed.");
    }

    // Remove duplicates and sort
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());

    return ports;
}

void flow_change(std::queue<std::pair<uint64_t, uint64_t>> qps_flow,
    std::vector<std::shared_ptr<TokenBucket>> rl_list,
    int verbosity,
    long c_count)
{
    auto flow = qps_flow.front();
    qps_flow.pop();
    if (verbosity) {
        if (qps_flow.size()) {
            std::cout << "QPS flow now " << flow.first << " for " << flow.second << "ms, flows left: "
                      << qps_flow.size() << std::endl;
        } else {
            std::cout << "QPS flow now " << flow.first << " until completion" << std::endl;
        }
    }
    for (auto &rl : rl_list) {
        *rl = TokenBucket(flow.first / c_count);
    }
    if (qps_flow.size() == 0)
        return;
    auto loop = uvw::Loop::getDefault();
    auto qps_timer = loop->resource<uvw::TimerHandle>();
    qps_timer->on<uvw::TimerEvent>([qps_flow, rl_list, verbosity, c_count](const auto &event, auto &handle) {
        handle.stop();
        flow_change(qps_flow, rl_list, verbosity, c_count);
    });
    qps_timer->start(uvw::TimerHandle::Time{flow.second}, uvw::TimerHandle::Time{0});
}

bool arg_exists(const char *needle, int argc, char *argv[])
{
    for (int i = 0; i < argc; i++) {
        if (std::string(needle) == std::string(argv[i])) {
            return true;
        }
    }
    return false;
}

void setupRoutes(const MetricsMgr *metricsManager, httplib::Server &svr)
{

    svr.Get(METRIC_ROUTE, [metricsManager](const httplib::Request &req, httplib::Response &res) {
        std::string out;
        try {
            out = metricsManager->toJSON();
            res.set_content(out, "text/json");
        } catch (const std::exception &e) {
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });
}

int main(int argc, char *argv[])
{

    std::map<std::string, docopt::value> args = docopt::docopt(USAGE,
        {argv + 1, argv + argc},
        true,           // show help if requested
        FLAME_VERSION); // version string

    if (args["-v"].asLong() > 3) {
        for (auto const &arg : args) {
            std::cout << arg.first << ": " << arg.second << std::endl;
        }
    }

    auto loop = uvw::Loop::getDefault();

    auto sigint = loop->resource<uvw::SignalHandle>();
    auto sigterm = loop->resource<uvw::SignalHandle>();
    std::shared_ptr<uvw::TimerHandle> run_timer;
    std::shared_ptr<uvw::TimerHandle> qgen_loop_timer;

    std::string output_file;
    if (args["-o"]) {
        output_file = args["-o"].asString();
    }

    // these defaults change based on protocol
    long s_delay = args["-d"].asLong();
    long b_count = args["-q"].asLong();
    long c_count = args["-c"].asLong();

    Protocol proto{Protocol::UDP};
    // note: tcptls is available as a deprecated alternative to dot
    if (args["-P"].asString() == "tcp" || args["-P"].asString() == "dot" || args["-P"].asString() == "tcptls" || args["-P"].asString() == "doh") {
        if (args["-P"].asString() == "dot" || args["-P"].asString() == "tcptls") {
            proto = Protocol::DOT;
        } else if (args["-P"].asString() == "doh") {
#ifdef DOH_ENABLE
            proto = Protocol::DOH;
#else
            std::cerr << "DNS over HTTPS (DoH) support is not enabled" << std::endl;
            return 1;
#endif
        } else {
            proto = Protocol::TCP;
        }
        if (!arg_exists("-d", argc, argv))
            s_delay = 1000;
        if (!arg_exists("-q", argc, argv))
            b_count = 100;
        if (!arg_exists("-c", argc, argv))
            c_count = 30;
    } else if (args["-P"].asString() == "udp") {
        proto = Protocol::UDP;
    } else {
        std::cerr << "protocol must be 'udp', 'tcp', dot' or 'doh'" << std::endl;
        return 1;
    }

    std::vector<unsigned int> ports;
    try {
        if (args["-p"]) {
            ports = parse_ports(args["-p"].asString());
        } else {
            // Set default port based on protocol
            if (proto == Protocol::DOT)
                ports.push_back(853);
#ifdef DOH_ENABLE
            else if (proto == Protocol::DOH)
                ports.push_back(443);
#endif
            else // UDP or TCP
                ports.push_back(53);
            if (args["-v"].asLong() > 1) {
                std::cout << "Using default port " << ports[0] << " for protocol " << args["-P"].asString() << std::endl;
            }
        }
    } catch (const std::runtime_error &e) {
        std::cerr << "Error parsing ports: " << e.what() << std::endl;
        return 1;
    }

#ifdef DOH_ENABLE
    HTTPMethod method{HTTPMethod::GET};
    if (args["-M"].asString() == "POST") {
        method = HTTPMethod::POST;
    }
#endif

    auto runtime_limit = args["-l"].asLong();

    auto family_s = args["-F"].asString();
    int family{0};
    if (family_s == "inet") {
        family = AF_INET;
    } else if (family_s == "inet6") {
        family = AF_INET6;
    } else {
        std::cerr << "internet family must be 'inet' or 'inet6'" << std::endl;
        return 1;
    }

    if (!args["-b"]) {
        if (family == AF_INET)
            args["-b"] = std::string("0.0.0.0");
        else
            args["-b"] = std::string("::0");
    }
    auto bind_ip = args["-b"].asString();
    auto bind_ip_request = loop->resource<uvw::GetAddrInfoReq>();
    auto bind_ip_resolved = bind_ip_request->addrInfoSync(bind_ip, "0");
    if (!bind_ip_resolved.first) {
        std::cerr << "unable to resolve bind ip address: " << bind_ip << std::endl;
        return 1;
    }

    std::vector<std::string> raw_target_list;
    if (args["TARGET"].asString() == "file" && args["--targets"]) {
        std::ifstream inFile(args["--targets"].asString());
        if (!inFile.is_open()) {
            std::cerr << "couldn't open targets file: " << args["--targets"].asString() << std::endl;
            return 1;
        }

        std::string line;
        while (getline(inFile, line)) {
            raw_target_list.push_back(line);
        }
        inFile.close();
    } else {
        raw_target_list = split(args["TARGET"].asString(), ',');
    }

    std::vector<Target> target_list;
    auto request = loop->resource<uvw::GetAddrInfoReq>();
    std::string first_port_str = std::to_string(ports[0]);

    for (uint i = 0; i < raw_target_list.size(); i++) {
        uvw::Addr addr;
        struct http_parser_url parsed = {};
        std::string url = raw_target_list[i];
        if (url.rfind("https://", 0) != 0) {
            url.insert(0, "https://");
        }
        int ret = http_parser_parse_url(url.c_str(), strlen(url.c_str()), 0, &parsed);
        if (ret != 0) {
            std::cerr << "could not parse url: " << url << std::endl;
            return 1;
        }
        std::string authority(&url[parsed.field_data[UF_HOST].off], parsed.field_data[UF_HOST].len);

        auto target_resolved = request->addrInfoSync(authority, first_port_str);
        if (!target_resolved.first) {
            std::cerr << "unable to resolve target address: " << authority << std::endl;
            if (raw_target_list[i] == "file") {
                std::cerr << "(did you mean to include --targets?)" << std::endl;
            }
            return 1;
        }
        addrinfo *node{target_resolved.second.get()};
        while (node && node->ai_family != family) {
            node = node->ai_next;
        }
        if (!node) {
            std::cerr << "name did not resolve to valid IP address for this inet family: " << raw_target_list[i] << std::endl;
            return 1;
        }

        if (family == AF_INET) {
            addr = uvw::details::address<uvw::IPv4>((struct sockaddr_in *)node->ai_addr);
        } else if (family == AF_INET6) {
            addr = uvw::details::address<uvw::IPv6>((struct sockaddr_in6 *)node->ai_addr);
        }
        target_list.push_back({&parsed, addr.ip, url});
    }

    long want_r_limit = args["-Q"].asLong();
    if (want_r_limit && want_r_limit < c_count) {
        std::cerr << "WARNING: QPS limit is less than concurrent senders, changing limit to " << c_count << std::endl;
        want_r_limit = c_count;
    }
    auto config = std::make_shared<Config>(
        args["-v"].asLong(),
        output_file,
        want_r_limit);

    std::shared_ptr<QueryGenerator> qgen;
    try {
        if (args["-f"]) {
            qgen = std::make_shared<FileQueryGenerator>(config, args["-f"].asString());
        } else if (args["-g"] && args["-g"].asString() == "numberqname") {
            qgen = std::make_shared<NumberNameQueryGenerator>(config);
        } else if (args["-g"] && args["-g"].asString() == "randompkt") {
            qgen = std::make_shared<RandomPktQueryGenerator>(config);
        } else if (args["-g"] && args["-g"].asString() == "randomqname") {
            qgen = std::make_shared<RandomQNameQueryGenerator>(config);
        } else if (args["-g"] && args["-g"].asString() == "randomlabel") {
            qgen = std::make_shared<RandomLabelQueryGenerator>(config);
        } else {
            qgen = std::make_shared<StaticQueryGenerator>(config);
        }
        qgen->set_args(args["GENOPTS"].asStringList());
        qgen->set_qclass(args["--class"].asString());
        qgen->set_loops(args["-n"].asLong());
        qgen->set_dnssec(args["--dnssec"].asBool());
        qgen->set_qname(args["-r"].asString());
        qgen->set_qtype(args["-T"].asString());
        qgen->init();
        if (!qgen->synthesizedQueries() && qgen->size() == 0) {
            throw std::runtime_error("no queries were generated");
        }
    } catch (const std::exception &e) {
        std::cerr << "generator error: " << e.what() << std::endl;
        return 1;
    }

    if (args["-R"].asBool()) {
        qgen->randomize();
    }

    std::string cmdline{};
    for (int i = 0; i < argc; i++) {
        cmdline.append(argv[i]);
        if (i != argc - 1) {
            cmdline.push_back(' ');
        }
    }
    auto metrics_mgr = std::make_shared<MetricsMgr>(loop, config, cmdline);
    httplib::Server svr;
    bool haveMetricServer(args["--http-srv"]);
    std::unique_ptr<std::thread> httpThread;

    if (haveMetricServer) {
        setupRoutes(metrics_mgr.get(), svr);
        auto port = args["--http-srv"].asLong();
        httpThread = std::make_unique<std::thread>([&svr, port] {
            std::cerr << "Metrics web server listening on http://localhost" << ":" << port << METRIC_ROUTE << std::endl;
            if (!svr.listen("localhost", port)) {
                throw std::runtime_error("unable to listen");
            }
        });
    }

    std::queue<std::pair<uint64_t, uint64_t>> qps_flow;
    std::vector<std::shared_ptr<TokenBucket>> rl_list;
    if (args["--qps-flow"]) {
        parse_flowspec(args["--qps-flow"].asString(), qps_flow, config->verbosity(), c_count);
    }

    auto traf_config = std::make_shared<TrafGenConfig>();
    traf_config->batch_count = b_count;
    traf_config->family = family;
    traf_config->bind_ip = bind_ip;
    traf_config->target_list = target_list;
    traf_config->port = static_cast<unsigned int>(args["-p"].asLong());
    traf_config->s_delay = s_delay;
    traf_config->protocol = proto;
#ifdef DOH_ENABLE
    traf_config->method = method;
#endif
    traf_config->r_timeout = args["-t"].asLong();

    std::vector<std::shared_ptr<TrafGen>> throwers;
    for (auto i = 0; i < c_count; i++) {
        std::shared_ptr<TokenBucket> rl;
        if (config->rate_limit()) {
            rl = std::make_shared<TokenBucket>(config->rate_limit() / static_cast<double>(c_count));
        } else if (args["--qps-flow"]) {
            rl = std::make_shared<TokenBucket>();
            rl_list.push_back(rl);
        }
        throwers.push_back(std::make_shared<TrafGen>(loop,
            metrics_mgr->create_trafgen_metrics(),
            config,
            traf_config,
            qgen,
            rl));
        throwers[i]->start();
    }
    if (args["--qps-flow"]) {
        flow_change(qps_flow, rl_list, config->verbosity(), c_count);
    }

    auto have_in_flight = [&throwers]() {
        for (const auto &i : throwers) {
            if (i->in_flight_cnt()) {
                return true;
            }
        }
        return false;
    };

    auto shutdown = [&]() {
        sigint->stop();
        sigterm->stop();
        if (run_timer.get())
            run_timer->stop();
        if (qgen_loop_timer.get())
            qgen_loop_timer->stop();
        for (auto &t : throwers) {
            t->stop();
        }
        metrics_mgr->stop();
        if (have_in_flight() && config->verbosity()) {
            std::cout << "stopping, waiting up to " << traf_config->r_timeout << "s for in flight to finish..." << std::endl;
        }
    };

    auto stop_traffic = [&](uvw::SignalEvent &, uvw::SignalHandle &) {
        shutdown();
    };

    if (runtime_limit != 0) {
        run_timer = loop->resource<uvw::TimerHandle>();
        run_timer->on<uvw::TimerEvent>([&shutdown](const auto &, auto &) { shutdown(); });
        run_timer->start(uvw::TimerHandle::Time{runtime_limit * 1000}, uvw::TimerHandle::Time{0});
    }

    if (qgen->loops()) {
        qgen_loop_timer = loop->resource<uvw::TimerHandle>();
        qgen_loop_timer->on<uvw::TimerEvent>([&qgen, &shutdown](const auto &, auto &) {
            if (qgen->finished()) {
                shutdown();
            } });
        qgen_loop_timer->start(uvw::TimerHandle::Time{500}, uvw::TimerHandle::Time{500});
    }

    sigint->on<uvw::SignalEvent>(stop_traffic);
    sigint->start(SIGINT);

    sigterm->on<uvw::SignalEvent>(stop_traffic);
    sigterm->start(SIGTERM);

    if (config->verbosity()) {
        std::cout << "binding traffic generators to " << traf_config->bind_ip << std::endl;
        std::cout << "flaming target(s) [";
        for (uint i = 0; i < 3; i++) {
            std::cout << traf_config->target_list[i].address;
            if (i == traf_config->target_list.size() - 1) {
                break;
            } else {
                std::cout << ", ";
            }
        }
        if (traf_config->target_list.size() > 3) {
            std::cout << "and " << traf_config->target_list.size() - 3 << " more";
        }
        std::cout << "] on port "
                  << args["-p"].asLong()
                  << " with " << c_count << " concurrent generators, each sending " << b_count
                  << " queries every " << s_delay << "ms on protocol " << args["-P"].asString()
                  << std::endl;
        std::cout << "query generator [" << qgen->name() << "] contains " << qgen->size() << " record(s)" << std::endl;
        if (args["-R"].asBool()) {
            std::cout << "query list randomized" << std::endl;
        }
        if (config->rate_limit()) {
            std::cout << "rate limit @ " << config->rate_limit() << " QPS (" << config->rate_limit() / static_cast<double>(c_count) << " QPS per concurrent sender)" << std::endl;
        }
    }

    metrics_mgr->start();
    loop->run();

    // break from loop with ^C or timer
    loop = nullptr;

    if (haveMetricServer) {
        svr.stop();
        httpThread->join();
    }

    // when loop is complete, finalize metrics
    metrics_mgr->finalize();

    return 0;
}
