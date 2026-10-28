// Airline Route Access Web Service — CIS 51 "Vibe Coding" assignment.
// C++ / cpp-httplib. Loads the provided OpenFlights-format data files
// (SB-airlines.txt, SB-airports.txt, SB-routes.txt) and serves a browser
// front end on port 8080 implementing the assignment's functionality:
//
//   Basic functionality
//     GET /airline?code=AA            1.1  full Airline entity for an IATA code
//     GET /airport?code=SFO           1.2  full Airport entity for an IATA code
//     GET /airline-airports?code=AA   2.1a airports reached by an airline,
//                                          ordered by # routes to/from each
//     GET /airport-airlines?code=DEN  2.1b airlines serving an airport,
//                                          ordered by # routes to/from it
//     GET /airlines                   2.2a all airlines ordered by IATA code
//     GET /airports                   2.2b all airports ordered by IATA code
//     GET /get-id                     2.3  site owner's De Anza student ID + name
//   Additional functionality
//     GET /get-owner                       student ID (site ownership check)
//     GET /get-code                        scrollable view of this C++ source
//     GET /one-hop?from=SFO&to=ORD         S->X->D direct-route pairs ordered
//                                          by total air miles + map of the
//                                          top 5 intermediate cities
//     GET /airline-routes?code=AA           JSON: an airline's airports +
//                                          route segments, for the homepage map
//   Homepage route map: pick an airline, see all of its routes on a map.
//   Sample-query helpers (from the assignment's verification examples)
//     GET /airlines-into?airport=DEN       airlines flying into an airport
//     GET /top-cities?airline=AA&n=3       an airline's top destination cities
//     GET /distance?from=SFO&to=ORD        great-circle distance in miles/km
//     GET /health                          {"ok":true}
//
// Data directory: argv[1], else env DATA_DIR, else "data".
// Source file for /get-code: env SOURCE_FILE, else "airline_service.cpp".
#include "httplib.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Site owner (assignment sections 2.3 "Get ID" and C.1 "Get Owner").
// >>> Bo: put your De Anza student ID number here — it is the only value
// >>> that has to be filled in by hand. It can also be supplied at runtime
// >>> with the STUDENT_ID / STUDENT_NAME environment variables.
// ---------------------------------------------------------------------------
const char* kStudentName = "Bo Engstrom";
const char* kStudentId = "20640900";

std::string studentName() {
    if (const char* e = std::getenv("STUDENT_NAME")) return e;
    return kStudentName;
}
std::string studentId() {
    if (const char* e = std::getenv("STUDENT_ID")) return e;
    return kStudentId;
}

// ---------- small CSV reader (handles quoted fields, "" escapes) ----------
std::vector<std::string> parseCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else inQuotes = false;
            } else cur += c;
        } else if (c == '"') {
            inQuotes = true;
        } else if (c == ',') {
            fields.push_back(cur); cur.clear();
        } else cur += c;
    }
    fields.push_back(cur);
    return fields;
}

int toInt(const std::string& s, int dflt = 0) {
    try { return std::stoi(s); } catch (...) { return dflt; }
}
double toDouble(const std::string& s, double dflt = 0) {
    try { return std::stod(s); } catch (...) { return dflt; }
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string htmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}
// Escape a string for use inside a JSON double-quoted string.
std::string jsonEscape(const std::string& s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                                  << static_cast<int>(c) << std::dec << std::setfill(' ');
                else out << static_cast<char>(c);
        }
    }
    return out.str();
}
// Format a double the way it appears in the data files (no trailing zeros).
std::string num(double v) {
    std::ostringstream o;
    o << std::setprecision(10) << v;
    return o.str();
}
// OpenFlights uses \N for "no value".
bool isNull(const std::string& s) { return s.empty() || s == "\\N"; }
std::string show(const std::string& s) { return isNull(s) ? std::string("(none)") : s; }

// ---------- OO data model: every element of each entity is kept ----------
struct Airline {
    int id = 0;                       // internal OpenFlights index (not shown)
    std::string name, alias, iata, icao, callsign, country, active;
};

struct Airport {
    int id = 0;                       // internal OpenFlights index (not shown)
    std::string name, city, country, iata, icao;
    std::string latRaw, lonRaw;       // exactly as in the data file
    double lat = 0, lon = 0;          // parsed, for distance math
    std::string altitude, tzOffset, dst, tzDb, type, source;
};

struct Route {
    std::string airlineIata;
    int airlineId = 0;
    std::string srcIata; int srcId = 0;
    std::string dstIata; int dstId = 0;
    std::string codeshare;            // "Y" or ""
    int stops = 0;
    std::string equipment;
};

class AirTravelDB {
public:
    bool load(const std::string& dir) {
        return loadAirlines(dir + "/SB-airlines.txt")
            && loadAirports(dir + "/SB-airports.txt")
            && loadRoutes(dir + "/SB-routes.txt");
    }

    const Airline* airlineByIata(const std::string& iata) const {
        auto it = airlineByIata_.find(upper(iata));
        return it == airlineByIata_.end() ? nullptr : it->second;
    }
    const Airline* airlineById(int id) const {
        auto it = airlinesById_.find(id);
        return it == airlinesById_.end() ? nullptr : it->second;
    }
    const Airport* airportByIata(const std::string& iata) const {
        auto it = airportByIata_.find(upper(iata));
        return it == airportByIata_.end() ? nullptr : it->second;
    }
    const Airport* airportById(int id) const {
        auto it = airportsById_.find(id);
        return it == airportsById_.end() ? nullptr : it->second;
    }
    // Find an airport by IATA code, else by city name (case-insensitive substring).
    const Airport* findAirport(const std::string& q) const {
        if (const Airport* a = airportByIata(q)) return a;
        std::string needle = lower(q);
        for (const auto& kv : airportsById_) {
            if (lower(kv.second->city).find(needle) != std::string::npos) return kv.second;
        }
        return nullptr;
    }

    // All entities, ordered by IATA code (entries without a code go last).
    const std::vector<const Airline*>& airlinesByIataOrder() const { return airlinesSorted_; }
    const std::vector<const Airport*>& airportsByIataOrder() const { return airportsSorted_; }

    // 2.1a: airports reached by an airline, counted by # routes to/from each.
    struct AirportCount { const Airport* airport; int routes; };
    std::vector<AirportCount> airportsForAirline(const Airline& al) const {
        std::map<int, int> counts;
        for (const Route& r : routes_) {
            if (r.airlineId != al.id) continue;
            counts[r.srcId]++;
            if (r.dstId != r.srcId) counts[r.dstId]++;
        }
        std::vector<AirportCount> out;
        for (const auto& kv : counts) {
            if (const Airport* ap = airportById(kv.first)) out.push_back({ap, kv.second});
        }
        std::sort(out.begin(), out.end(), [](const AirportCount& a, const AirportCount& b) {
            return a.routes != b.routes ? a.routes > b.routes : a.airport->iata < b.airport->iata;
        });
        return out;
    }

    // Distinct route pairs (src -> dst) flown by an airline, for the
    // homepage route map. Each pair appears once, even if the data file
    // lists it more than once. Pairs with a missing airport, or an airport
    // with no coordinates (0,0), are skipped.
    struct RoutePair { const Airport* src; const Airport* dst; };
    std::vector<RoutePair> routesForAirline(const Airline& al) const {
        std::set<std::pair<int, int>> seen;
        std::vector<RoutePair> out;
        for (const Route& r : routes_) {
            if (r.airlineId != al.id) continue;
            if (!seen.insert({r.srcId, r.dstId}).second) continue;
            const Airport* s = airportById(r.srcId);
            const Airport* d = airportById(r.dstId);
            if (!s || !d) continue;
            if ((s->lat == 0 && s->lon == 0) || (d->lat == 0 && d->lon == 0)) continue;
            out.push_back({s, d});
        }
        return out;
    }

    // 2.1b: airlines with routes that include an airport, counted by # routes.
    struct AirlineCount { const Airline* airline; int routes; };
    std::vector<AirlineCount> airlinesForAirport(const Airport& ap) const {
        std::map<int, int> counts;
        for (const Route& r : routes_) {
            if (r.srcId == ap.id || r.dstId == ap.id) counts[r.airlineId]++;
        }
        std::vector<AirlineCount> out;
        for (const auto& kv : counts) {
            if (const Airline* al = airlineById(kv.first)) out.push_back({al, kv.second});
        }
        std::sort(out.begin(), out.end(), [](const AirlineCount& a, const AirlineCount& b) {
            return a.routes != b.routes ? a.routes > b.routes : a.airline->iata < b.airline->iata;
        });
        return out;
    }

    // Distinct airlines flying INTO the given airport (sample-query helper).
    std::vector<const Airline*> airlinesInto(const Airport& ap) const {
        std::set<int> seen;
        std::vector<const Airline*> out;
        for (const Route& r : routes_) {
            if (r.dstId == ap.id && seen.insert(r.airlineId).second) {
                if (const Airline* al = airlineById(r.airlineId)) out.push_back(al);
            }
        }
        std::sort(out.begin(), out.end(),
                  [](const Airline* a, const Airline* b) { return a->name < b->name; });
        return out;
    }

    // Top destination cities for an airline: (airport, route count).
    struct CityCount { const Airport* airport; int count; };
    std::vector<CityCount> topCitiesInto(const Airline& al, size_t n) const {
        std::map<int, int> counts;
        for (const Route& r : routes_) {
            if (r.airlineId == al.id) counts[r.dstId]++;
        }
        std::vector<CityCount> out;
        for (const auto& kv : counts) {
            if (const Airport* ap = airportById(kv.first)) out.push_back({ap, kv.second});
        }
        std::sort(out.begin(), out.end(), [](const CityCount& a, const CityCount& b) {
            return a.count != b.count ? a.count > b.count : a.airport->city < b.airport->city;
        });
        if (out.size() > n) out.resize(n);
        return out;
    }

    // One-hop report: intermediate airports X with a direct (0-stop) route
    // S->X and a direct route X->D. Aggregated per X: number of route pairs
    // and total air miles S->X->D, ordered by total miles.
    struct Hop {
        const Airport* via;
        int pairs;                    // # of (S->X, X->D) route combinations
        double miles;                 // dist(S,X) + dist(X,D)
        std::string leg1Airlines;     // airline codes seen on S->X
        std::string leg2Airlines;     // airline codes seen on X->D
    };
    std::vector<Hop> oneHop(const Airport& src, const Airport& dst) const {
        std::map<int, std::set<std::string>> leg1, leg2;
        for (const Route& r : routes_) {
            if (r.stops != 0) continue;
            if (r.srcId == src.id && r.dstId != src.id && r.dstId != dst.id)
                leg1[r.dstId].insert(r.airlineIata);
            if (r.dstId == dst.id && r.srcId != src.id && r.srcId != dst.id)
                leg2[r.srcId].insert(r.airlineIata);
        }
        std::vector<Hop> out;
        for (const auto& kv : leg1) {
            auto it = leg2.find(kv.first);
            if (it == leg2.end()) continue;
            const Airport* via = airportById(kv.first);
            if (!via) continue;
            auto join = [](const std::set<std::string>& s) {
                std::string r;
                for (const auto& x : s) { if (!r.empty()) r += ", "; r += x; }
                return r;
            };
            Hop h;
            h.via = via;
            h.pairs = static_cast<int>(kv.second.size() * it->second.size());
            h.miles = distanceMiles(src, *via) + distanceMiles(*via, dst);
            h.leg1Airlines = join(kv.second);
            h.leg2Airlines = join(it->second);
            out.push_back(h);
        }
        std::sort(out.begin(), out.end(), [](const Hop& a, const Hop& b) {
            return a.miles != b.miles ? a.miles < b.miles : a.via->iata < b.via->iata;
        });
        return out;
    }
    bool hasDirectRoute(const Airport& a, const Airport& b) const {
        for (const Route& r : routes_)
            if (r.stops == 0 && r.srcId == a.id && r.dstId == b.id) return true;
        return false;
    }

    static double distanceMiles(const Airport& a, const Airport& b) {
        const double R = 3958.8; // Earth radius, miles
        double dLat = (b.lat - a.lat) * M_PI / 180.0;
        double dLon = (b.lon - a.lon) * M_PI / 180.0;
        double la1 = a.lat * M_PI / 180.0, la2 = b.lat * M_PI / 180.0;
        double h = std::sin(dLat / 2) * std::sin(dLat / 2) +
                   std::cos(la1) * std::cos(la2) * std::sin(dLon / 2) * std::sin(dLon / 2);
        return 2 * R * std::asin(std::sqrt(h));
    }

    size_t airlineCount() const { return airlines_.size(); }
    size_t airportCount() const { return airports_.size(); }
    size_t routeCount() const { return routes_.size(); }

private:
    bool loadAirlines(const std::string& path) {
        std::ifstream in(path);
        if (!in) { std::cerr << "cannot open " << path << "\n"; return false; }
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            auto f = parseCsvLine(line);
            if (f.size() < 8) continue;
            Airline a;
            a.id = toInt(f[0]); a.name = f[1]; a.alias = f[2]; a.iata = f[3];
            a.icao = f[4]; a.callsign = f[5]; a.country = f[6]; a.active = f[7];
            airlines_.push_back(a);
        }
        for (const Airline& a : airlines_) {
            airlinesById_[a.id] = &a;
            if (!isNull(a.iata)) airlineByIata_[upper(a.iata)] = &a;
        }
        airlinesSorted_.clear();
        for (const Airline& a : airlines_) airlinesSorted_.push_back(&a);
        std::sort(airlinesSorted_.begin(), airlinesSorted_.end(),
                  [](const Airline* x, const Airline* y) {
                      std::string a = isNull(x->iata) ? "~" : upper(x->iata);
                      std::string b = isNull(y->iata) ? "~" : upper(y->iata);
                      return a < b;
                  });
        return true;
    }
    bool loadAirports(const std::string& path) {
        std::ifstream in(path);
        if (!in) { std::cerr << "cannot open " << path << "\n"; return false; }
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            auto f = parseCsvLine(line);
            if (f.size() < 14) continue;
            Airport a;
            a.id = toInt(f[0]); a.name = f[1]; a.city = f[2]; a.country = f[3];
            a.iata = f[4]; a.icao = f[5];
            a.latRaw = f[6]; a.lonRaw = f[7];
            a.lat = toDouble(f[6]); a.lon = toDouble(f[7]);
            a.altitude = f[8]; a.tzOffset = f[9]; a.dst = f[10]; a.tzDb = f[11];
            a.type = f[12]; a.source = f[13];
            airports_.push_back(a);
        }
        for (const Airport& a : airports_) {
            airportsById_[a.id] = &a;
            if (!isNull(a.iata)) airportByIata_[upper(a.iata)] = &a;
        }
        airportsSorted_.clear();
        for (const Airport& a : airports_) airportsSorted_.push_back(&a);
        std::sort(airportsSorted_.begin(), airportsSorted_.end(),
                  [](const Airport* x, const Airport* y) {
                      std::string a = isNull(x->iata) ? "~" : upper(x->iata);
                      std::string b = isNull(y->iata) ? "~" : upper(y->iata);
                      return a < b;
                  });
        return true;
    }
    bool loadRoutes(const std::string& path) {
        std::ifstream in(path);
        if (!in) { std::cerr << "cannot open " << path << "\n"; return false; }
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            auto f = parseCsvLine(line);
            if (f.size() < 9) continue;
            Route r;
            r.airlineIata = f[0];
            r.airlineId = toInt(f[1]);
            r.srcIata = f[2]; r.srcId = toInt(f[3]);
            r.dstIata = f[4]; r.dstId = toInt(f[5]);
            r.codeshare = f[6];
            r.stops = toInt(f[7]);
            r.equipment = f[8];
            routes_.push_back(r);
        }
        return true;
    }

    std::vector<Airline> airlines_;
    std::vector<Airport> airports_;
    std::map<std::string, const Airline*> airlineByIata_;
    std::map<int, const Airline*> airlinesById_;
    std::map<std::string, const Airport*> airportByIata_;
    std::map<int, const Airport*> airportsById_;
    std::vector<const Airline*> airlinesSorted_;
    std::vector<const Airport*> airportsSorted_;
    std::vector<Route> routes_;
};

// ---------- HTML helpers ----------
const char* kCss =
    "body{font-family:ui-sans-serif,system-ui,-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;margin:0;background:#edf3f8;color:#1c2733;line-height:1.55}"
    "header{background:linear-gradient(115deg,#061a3a 0%,#0b3d66 45%,#0e7490 72%,#38bdf8 100%);color:#fff;padding:1.7em 1.5em 1.5em}"
    "header h1{margin:.1em 0;font-size:1.65em;letter-spacing:-.02em}"
    "nav{background:#072f4f;padding:.55em 1.5em;position:sticky;top:0;z-index:10;box-shadow:0 2px 8px rgba(11,61,102,.3)}"
    "nav a{color:#cfe6f5;margin-right:.3em;padding:.35em .8em;border-radius:999px;text-decoration:none;font-size:.93em;background:rgba(255,255,255,.08);transition:background .15s}"
    "nav a:hover{background:rgba(255,255,255,.18);color:#fff}"
    "main{max-width:72em;margin:0 auto;padding:1.4em 1.5em 3em}"
    "h2{margin:1.6em 0 .6em;color:#0b3d66;font-size:1.25em;letter-spacing:-.01em}"
    "h2:first-child{margin-top:.2em}"
    "a{color:#0b6fa4}"
    "table{border-collapse:separate;border-spacing:0;background:#fff;width:100%;border-radius:14px;overflow:hidden;box-shadow:0 4px 14px rgba(11,61,102,.08)}"
    "td,th{border-bottom:1px solid #dbe4ec;padding:.5em .8em;text-align:left;font-size:.93em}"
    "th{background:#e6f1f8;color:#33506a;font-size:.8em;text-transform:uppercase;letter-spacing:.05em;white-space:nowrap}"
    "tr:last-child td{border-bottom:0}"
    "tbody tr:hover{background:#f0f8fd}"
    "form{margin:.8em 0}"
    "input{padding:.5em .7em;border:1px solid #b8c9d6;border-radius:10px;font-size:.95em;background:#fff}"
    "input:focus{outline:2px solid #38bdf8;outline-offset:1px;border-color:#0ea5e9}"
    "select{padding:.5em .7em;border:1px solid #b8c9d6;border-radius:10px;font-size:.95em;background:#fff}"
    "button{padding:.5em 1.2em;background:linear-gradient(135deg,#0284c7,#0ea5e9);color:#fff;border:0;border-radius:10px;cursor:pointer;font-weight:600;font-size:.95em;box-shadow:0 1px 3px rgba(11,61,102,.3)}"
    "button:hover{background:#0369a1}"
    ".scroll{max-height:65vh;overflow:auto;border:1px solid #dbe4ec;border-radius:14px;box-shadow:0 4px 14px rgba(11,61,102,.08);background:#fff}"
    ".scroll table{box-shadow:none;border-radius:0}"
    ".scroll th{position:sticky;top:0}"
    "#map{border-radius:16px;box-shadow:0 8px 24px rgba(11,61,102,.18)}"
    "pre.code{background:#10222e;color:#d7e8f0;padding:1em;border-radius:10px;overflow:auto}"
    ".note{color:#5a6b78;font-size:.92em}"
    ".stats{display:grid;grid-template-columns:repeat(3,1fr);gap:1em;margin:0 0 1.4em}"
    ".stat{background:#fff;border:1px solid #dbe4ec;border-radius:16px;padding:1.1em .5em;text-align:center;box-shadow:0 6px 18px rgba(11,61,102,.10);transition:transform .15s}"
    ".stat:hover{transform:translateY(-2px)}"
    ".stat-num{font-size:1.9em;font-weight:700;color:#0b3d66;line-height:1.1}"
    ".stat:nth-child(1) .stat-num::before{content:'✈ '}"
    ".stat:nth-child(2) .stat-num::before{content:'🌍 '}"
    ".stat:nth-child(3) .stat-num::before{content:'🛫 '}"
    ".stat-label{color:#5a6b78;font-size:.9em;margin-top:.2em}"
    ".hero{display:flex;align-items:center;gap:1em;background:linear-gradient(120deg,#0b3d66,#0e7490 55%,#38bdf8);color:#fff;border-radius:16px;padding:1.2em 1.5em;margin:0 0 1.2em;box-shadow:0 8px 24px rgba(11,61,102,.25)}"
    "h2.hero-title{color:#fff;margin:0 0 .25em;font-size:1.5em}"
    ".hero-tag{color:#dbeeff;margin:0;font-size:.98em}"
    ".hero svg{flex:0 0 auto;opacity:.95}"
    "@media(max-width:640px){.hero{flex-direction:column;align-items:flex-start}.hero svg{display:none}.stats{grid-template-columns:1fr}}"
    "footer{text-align:center;color:#5a6b78;font-size:.85em;padding:1.5em 1em 2em;border-top:1px solid #dbe4ec}";

std::string page(const std::string& title, const std::string& body) {
    return "<!doctype html><html><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<link rel='icon' href='data:image/svg+xml,%3Csvg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 100 100%22%3E%3Ctext y=%22.9em%22 font-size=%2290%22%3E✈️%3C/text%3E%3C/svg%3E'>"
           "<title>" + title +
           "</title><style>" + kCss + "</style></head><body>"
           "<header><h1>✈ Aviation Data Front End</h1>"
           "<div class='note' style='color:#cfe6f5'>CIS 51 Vibe Coding — OpenFlights Airline Route Access</div></header>"
           "<nav><a href='/'>Home</a><a href='/airlines'>All Airlines</a>"
           "<a href='/airports'>All Airports</a><a href='/get-id'>Get ID</a>"
           "<a href='/get-owner'>Get Owner</a><a href='/get-code'>Get Code</a></nav>"
           "<main>" + body + "</main>"
           "<footer>Built by Bo Engstrom · CIS 51 Vibe Coding · De Anza College</footer></body></html>";
}

std::string backLink() { return "<p><a href='/'>← Back to home</a></p>"; }

// Full entity tables: every element of the entity is shown.
std::string airlineTable(const Airline& a) {
    std::ostringstream b;
    b << "<table>"
      << "<tr><th>Name</th><td>" << htmlEscape(a.name) << "</td></tr>"
      << "<tr><th>Alias</th><td>" << htmlEscape(show(a.alias)) << "</td></tr>"
      << "<tr><th>IATA</th><td>" << htmlEscape(show(a.iata)) << "</td></tr>"
      << "<tr><th>ICAO</th><td>" << htmlEscape(show(a.icao)) << "</td></tr>"
      << "<tr><th>Callsign</th><td>" << htmlEscape(show(a.callsign)) << "</td></tr>"
      << "<tr><th>Country</th><td>" << htmlEscape(a.country) << "</td></tr>"
      << "<tr><th>Active</th><td>" << htmlEscape(a.active) << "</td></tr>"
      << "</table>";
    return b.str();
}
std::string airportTable(const Airport& a) {
    std::ostringstream b;
    b << "<table>"
      << "<tr><th>Name</th><td>" << htmlEscape(a.name) << "</td></tr>"
      << "<tr><th>City</th><td>" << htmlEscape(a.city) << "</td></tr>"
      << "<tr><th>Country</th><td>" << htmlEscape(a.country) << "</td></tr>"
      << "<tr><th>IATA</th><td>" << htmlEscape(show(a.iata)) << "</td></tr>"
      << "<tr><th>ICAO</th><td>" << htmlEscape(show(a.icao)) << "</td></tr>"
      << "<tr><th>Latitude</th><td>" << htmlEscape(a.latRaw) << "</td></tr>"
      << "<tr><th>Longitude</th><td>" << htmlEscape(a.lonRaw) << "</td></tr>"
      << "<tr><th>Altitude (ft)</th><td>" << htmlEscape(a.altitude) << "</td></tr>"
      << "<tr><th>Timezone (UTC offset)</th><td>" << htmlEscape(a.tzOffset) << "</td></tr>"
      << "<tr><th>DST</th><td>" << htmlEscape(a.dst) << "</td></tr>"
      << "<tr><th>Tz database timezone</th><td>" << htmlEscape(a.tzDb) << "</td></tr>"
      << "<tr><th>Type</th><td>" << htmlEscape(a.type) << "</td></tr>"
      << "<tr><th>Source</th><td>" << htmlEscape(a.source) << "</td></tr>"
      << "</table>";
    return b.str();
}

// One row of a report: all elements of an airport plus its "# routes" value.
void airportRow(std::ostringstream& b, const Airport& a, int routes) {
    b << "<tr><td><b>" << routes << "</b></td>"
      << "<td>" << htmlEscape(show(a.iata)) << "</td>"
      << "<td>" << htmlEscape(a.name) << "</td>"
      << "<td>" << htmlEscape(a.city) << "</td>"
      << "<td>" << htmlEscape(a.country) << "</td>"
      << "<td>" << htmlEscape(show(a.icao)) << "</td>"
      << "<td>" << htmlEscape(a.latRaw) << "</td>"
      << "<td>" << htmlEscape(a.lonRaw) << "</td>"
      << "<td>" << htmlEscape(a.altitude) << "</td>"
      << "<td>" << htmlEscape(a.tzOffset) << "</td>"
      << "<td>" << htmlEscape(a.dst) << "</td>"
      << "<td>" << htmlEscape(a.tzDb) << "</td>"
      << "<td>" << htmlEscape(a.type) << "</td>"
      << "<td>" << htmlEscape(a.source) << "</td></tr>";
}
const char* kAirportHead =
    "<tr><th># routes</th><th>IATA</th><th>Name</th><th>City</th><th>Country</th>"
    "<th>ICAO</th><th>Latitude</th><th>Longitude</th><th>Altitude (ft)</th>"
    "<th>TZ offset</th><th>DST</th><th>Tz database</th><th>Type</th><th>Source</th></tr>";

void airlineRow(std::ostringstream& b, const Airline& a, int routes) {
    b << "<tr>";
    if (routes >= 0) b << "<td><b>" << routes << "</b></td>";
    b << "<td>" << htmlEscape(show(a.iata)) << "</td>"
      << "<td>" << htmlEscape(a.name) << "</td>"
      << "<td>" << htmlEscape(show(a.alias)) << "</td>"
      << "<td>" << htmlEscape(show(a.icao)) << "</td>"
      << "<td>" << htmlEscape(show(a.callsign)) << "</td>"
      << "<td>" << htmlEscape(a.country) << "</td>"
      << "<td>" << htmlEscape(a.active) << "</td></tr>";
}
const char* kAirlineHeadWithCount =
    "<tr><th># routes</th><th>IATA</th><th>Name</th><th>Alias</th><th>ICAO</th>"
    "<th>Callsign</th><th>Country</th><th>Active</th></tr>";
const char* kAirlineHead =
    "<tr><th>IATA</th><th>Name</th><th>Alias</th><th>ICAO</th>"
    "<th>Callsign</th><th>Country</th><th>Active</th></tr>";
const char* kAirportHeadNoCount =
    "<tr><th>IATA</th><th>Name</th><th>City</th><th>Country</th>"
    "<th>ICAO</th><th>Latitude</th><th>Longitude</th><th>Altitude (ft)</th>"
    "<th>TZ offset</th><th>DST</th><th>Tz database</th><th>Type</th><th>Source</th></tr>";
void airportRowNoCount(std::ostringstream& b, const Airport& a) {
    b << "<tr>"
      << "<td>" << htmlEscape(show(a.iata)) << "</td>"
      << "<td>" << htmlEscape(a.name) << "</td>"
      << "<td>" << htmlEscape(a.city) << "</td>"
      << "<td>" << htmlEscape(a.country) << "</td>"
      << "<td>" << htmlEscape(show(a.icao)) << "</td>"
      << "<td>" << htmlEscape(a.latRaw) << "</td>"
      << "<td>" << htmlEscape(a.lonRaw) << "</td>"
      << "<td>" << htmlEscape(a.altitude) << "</td>"
      << "<td>" << htmlEscape(a.tzOffset) << "</td>"
      << "<td>" << htmlEscape(a.dst) << "</td>"
      << "<td>" << htmlEscape(a.tzDb) << "</td>"
      << "<td>" << htmlEscape(a.type) << "</td>"
      << "<td>" << htmlEscape(a.source) << "</td></tr>";
}

std::string readTextFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

int main(int argc, char** argv) {
    std::string dir = "data";
    if (argc > 1) dir = argv[1];
    else if (const char* e = std::getenv("DATA_DIR")) dir = e;

    std::string sourcePath = "airline_service.cpp";
    if (const char* e = std::getenv("SOURCE_FILE")) sourcePath = e;

    AirTravelDB db;
    if (!db.load(dir)) {
        std::cerr << "Failed to load data from " << dir << "\n";
        return 1;
    }
    std::cout << "Loaded " << db.airlineCount() << " airlines, " << db.airportCount()
              << " airports, " << db.routeCount() << " routes from " << dir << "\n";

    httplib::Server svr;

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Get("/", [&db](const httplib::Request&, httplib::Response& res) {
        std::ostringstream b;
        // Hero band: airlines / global-travel theme, decorative inline SVG
        // flight arc with a plane — no new external dependencies.
        b << "<div class='hero'><div><h2 class='hero-title'>Explore airline routes around the world</h2>"
          << "<p class='hero-tag'>Every airline, every airport, every route — pick an airline below and watch its whole network light up on the map.</p></div>"
          << "<svg width='300' height='110' viewBox='0 0 300 110' aria-hidden='true'>"
          << "<path d='M15 95 Q 150 -10 285 80' fill='none' stroke='rgba(255,255,255,.75)' stroke-width='2' stroke-dasharray='7 8'/>"
          << "<circle cx='15' cy='95' r='5' fill='#fff'/><circle cx='285' cy='80' r='5' fill='#fff'/>"
          << "<text x='256' y='64' font-size='26' fill='#fff'>✈</text>"
          << "</svg></div>";
        b << "<div class='stats'>"
          << "<div class='stat'><div class='stat-num'>" << db.airlineCount() << "</div><div class='stat-label'>Airlines</div></div>"
          << "<div class='stat'><div class='stat-num'>" << db.airportCount() << "</div><div class='stat-label'>Airports</div></div>"
          << "<div class='stat'><div class='stat-num'>" << db.routeCount() << "</div><div class='stat-label'>Routes</div></div>"
          << "</div>";
        // Route map: pick an airline, see all of its routes on the map.
        b << "<h2>Route map — pick an airline</h2>"
          << "<p class='note'>Every route of the airline you select, drawn on the map.</p>"
          << "<form onsubmit='return false'><label for='airlineSelect'>Airline </label>"
          << "<select id='airlineSelect' name='airlineSelect'>";
        for (const Airline* al : db.airlinesByIataOrder()) {
            if (isNull(al->iata)) continue;
            b << "<option value='" << htmlEscape(al->iata) << "'"
              << (upper(al->iata) == "SB" ? " selected" : "") << ">"
              << htmlEscape(al->iata) << " — " << htmlEscape(al->name) << "</option>";
        }
        b << "</select></form>"
          << "<div id='map' style='height:420px;border:1px solid #c9d4dd'></div>"
          << "<p class='note' id='mapNote'>Loading routes…</p>"
          << "<link rel='stylesheet' href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
          << "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
          << "<script>window.addEventListener('load',function(){"
          << "var sel=document.getElementById('airlineSelect');"
          << "var note=document.getElementById('mapNote');"
          << "var map=L.map('map',{center:[20,0],zoom:2,minZoom:1,scrollWheelZoom:false});"
          << "L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',"
          << "{attribution:'© OpenStreetMap contributors'}).addTo(map);"
          << "var layer=L.layerGroup().addTo(map);"
          << "function show(code){"
          << "note.textContent='Loading routes…';"
          << "fetch('/airline-routes?code='+encodeURIComponent(code)).then(function(r){"
          << "if(!r.ok)throw new Error('not found');return r.json();}).then(function(d){"
          << "layer.clearLayers();var pts=[];var byIata={};"
          << "d.airports.forEach(function(a){byIata[a.iata]=a;"
          << "var m=L.circleMarker([a.lat,a.lon],{radius:3,color:'#0b3d66',weight:1,"
          << "fillColor:'#0b6fa4',fillOpacity:.9}).addTo(layer);"
          << "m.bindPopup(a.iata+' — '+a.city);pts.push([a.lat,a.lon]);});"
          << "d.segments.forEach(function(s){var a=byIata[s[0]],c=byIata[s[1]];if(!a||!c)return;"
          << "L.polyline([[a.lat,a.lon],[c.lat,c.lon]],{color:'#0b6fa4',weight:1,opacity:.45}).addTo(layer);});"
          << "note.textContent=d.airline.name+' ('+d.airline.iata+'): '+d.routes.toLocaleString()"
          << "+' routes, '+d.airports.length+' airports.';"
          << "if(pts.length){map.fitBounds(pts,{padding:[30,30]});}"
          << "map.invalidateSize();}).catch(function(){"
          << "note.textContent='Could not load the route map. Try the “Airports reached by this airline” report below.';});}"
          << "sel.addEventListener('change',function(){show(sel.value);});"
          << "show(sel.value);"
          << "setTimeout(function(){map.invalidateSize();},500);});</script>";
        b << "<h2>1. Look up one airline or airport (full entity)</h2>"
          << "<form action='/airline'><input name='code' placeholder='Airline IATA, e.g. AA' required>"
          << "<button>Find airline</button></form>"
          << "<form action='/airport'><input name='code' placeholder='Airport IATA, e.g. SFO' required>"
          << "<button>Find airport</button></form>"
          << "<h2>2. Reports ordered by # routes</h2>"
          << "<form action='/airline-airports'><input name='code' placeholder='Airline IATA, e.g. AA' required>"
          << "<button>Airports reached by this airline</button></form>"
          << "<form action='/airport-airlines'><input name='code' placeholder='Airport IATA, e.g. DEN' required>"
          << "<button>Airlines serving this airport</button></form>"
          << "<h2>3. Full reports ordered by IATA code</h2>"
          << "<p><a href='/airlines'>All airlines</a> · <a href='/airports'>All airports</a></p>"
          << "<h2>4. One-hop report (via one intermediate airport)</h2>"
          << "<form action='/one-hop'><input name='from' placeholder='From, e.g. SFO' required>"
          << "<input name='to' placeholder='To, e.g. ORD' required>"
          << "<button>Find one-hop routes</button></form>"
          << "<h2>5. Sample queries</h2>"
          << "<form action='/airlines-into'><input name='airport' placeholder='DEN or Denver' required>"
          << "<button>Airlines flying into an airport</button></form>"
          << "<form action='/top-cities'><input name='airline' placeholder='AA' required>"
          << "<input name='n' type='number' value='3' min='1' max='25' style='width:4em'>"
          << "<button>Top cities an airline flies into</button></form>"
          << "<form action='/distance'><input name='from' placeholder='SFO' required>"
          << "<input name='to' placeholder='ORD' required>"
          << "<button>Distance between two airports</button></form>"
          << "<h2>6. Site ownership &amp; source</h2>"
          << "<p><a href='/get-id'>Get ID</a> · <a href='/get-owner'>Get Owner</a> · "
          << "<a href='/get-code'>Get Code (the C++ source of this site)</a></p>";
        res.set_content(page("Aviation Data Front End", b.str()), "text/html");
    });

    // 1.1 Individual airline retrieval — all elements.
    svr.Get("/airline", [&db](const httplib::Request& req, httplib::Response& res) {
        std::string code = req.get_param_value("code");
        const Airline* al = db.airlineByIata(code);
        std::ostringstream b;
        if (!al) {
            b << "<h2>Not found</h2><p>No airline with IATA code '" << htmlEscape(code) << "'.</p>";
        } else {
            b << "<h2>Airline: " << htmlEscape(al->name) << "</h2>" << airlineTable(*al);
        }
        b << backLink();
        res.set_content(page("Airline", b.str()), "text/html");
    });

    // 1.2 Individual airport retrieval — all elements.
    svr.Get("/airport", [&db](const httplib::Request& req, httplib::Response& res) {
        std::string code = req.get_param_value("code");
        const Airport* ap = db.airportByIata(code);
        std::ostringstream b;
        if (!ap) {
            b << "<h2>Not found</h2><p>No airport with IATA code '" << htmlEscape(code) << "'.</p>";
        } else {
            b << "<h2>Airport: " << htmlEscape(ap->name) << "</h2>" << airportTable(*ap);
        }
        b << backLink();
        res.set_content(page("Airport", b.str()), "text/html");
    });

    // 2.1a Airline -> airports reached, ordered by # routes to/from each.
    svr.Get("/airline-airports", [&db](const httplib::Request& req, httplib::Response& res) {
        std::string code = req.get_param_value("code");
        const Airline* al = db.airlineByIata(code);
        std::ostringstream b;
        if (!al) {
            b << "<h2>Not found</h2><p>No airline with IATA code '" << htmlEscape(code) << "'.</p>";
        } else {
            auto rows = db.airportsForAirline(*al);
            b << "<h2>" << htmlEscape(al->name) << " (" << htmlEscape(al->iata)
              << ") — airports reached, by # routes</h2>"
              << "<p class='note'>" << rows.size()
              << " airport(s). “# routes” = routes of this airline to or from the airport.</p>"
              << "<div class='scroll'><table>" << kAirportHead;
            for (const auto& r : rows) airportRow(b, *r.airport, r.routes);
            b << "</table></div>";
        }
        b << backLink();
        res.set_content(page("Airline airports by # routes", b.str()), "text/html");
    });

    // 2.1b Airport -> airlines serving it, ordered by # routes to/from it.
    svr.Get("/airport-airlines", [&db](const httplib::Request& req, httplib::Response& res) {
        std::string code = req.get_param_value("code");
        const Airport* ap = db.airportByIata(code);
        std::ostringstream b;
        if (!ap) {
            b << "<h2>Not found</h2><p>No airport with IATA code '" << htmlEscape(code) << "'.</p>";
        } else {
            auto rows = db.airlinesForAirport(*ap);
            b << "<h2>" << htmlEscape(ap->name) << " (" << htmlEscape(ap->iata)
              << ") — airlines, by # routes</h2>"
              << "<p class='note'>" << rows.size()
              << " airline(s). “# routes” = routes of the airline to or from this airport.</p>"
              << "<div class='scroll'><table>" << kAirlineHeadWithCount;
            for (const auto& r : rows) airlineRow(b, *r.airline, r.routes);
            b << "</table></div>";
        }
        b << backLink();
        res.set_content(page("Airport airlines by # routes", b.str()), "text/html");
    });

    // Homepage route map data: JSON with an airline's airports (with
    // coordinates) and its distinct route segments as IATA pairs.
    svr.Get("/airline-routes", [&db](const httplib::Request& req, httplib::Response& res) {
        std::string code = req.get_param_value("code");
        const Airline* al = db.airlineByIata(code);
        if (!al) {
            res.status = 404;
            res.set_content("{\"error\":\"No airline with IATA code '" + jsonEscape(code) + "'.\"}",
                            "application/json");
            return;
        }
        auto pairs = db.routesForAirline(*al);
        std::map<int, const Airport*> airports;
        for (const auto& p : pairs) {
            airports[p.src->id] = p.src;
            airports[p.dst->id] = p.dst;
        }
        std::ostringstream j;
        j << "{\"airline\":{\"iata\":\"" << jsonEscape(al->iata)
          << "\",\"name\":\"" << jsonEscape(al->name)
          << "\"},\"routes\":" << pairs.size() << ",\"airports\":[";
        bool first = true;
        for (const auto& kv : airports) {
            const Airport* a = kv.second;
            if (!first) j << ",";
            first = false;
            j << "{\"iata\":\"" << jsonEscape(a->iata)
              << "\",\"city\":\"" << jsonEscape(a->city)
              << "\",\"lat\":" << num(a->lat) << ",\"lon\":" << num(a->lon) << "}";
        }
        j << "],\"segments\":[";
        first = true;
        for (const auto& p : pairs) {
            if (!first) j << ",";
            first = false;
            j << "[\"" << jsonEscape(p.src->iata) << "\",\"" << jsonEscape(p.dst->iata) << "\"]";
        }
        j << "]}";
        res.set_content(j.str(), "application/json");
    });

    // 2.2a All airlines ordered by IATA code.
    svr.Get("/airlines", [&db](const httplib::Request&, httplib::Response& res) {
        std::ostringstream b;
        b << "<h2>All airlines, ordered by IATA code</h2>"
          << "<p class='note'>" << db.airlineCount() << " airline(s).</p>"
          << "<div class='scroll'><table>" << kAirlineHead;
        for (const Airline* al : db.airlinesByIataOrder()) airlineRow(b, *al, -1);
        b << "</table></div>" << backLink();
        res.set_content(page("All airlines", b.str()), "text/html");
    });

    // 2.2b All airports ordered by IATA code.
    svr.Get("/airports", [&db](const httplib::Request&, httplib::Response& res) {
        std::ostringstream b;
        b << "<h2>All airports, ordered by IATA code</h2>"
          << "<p class='note'>" << db.airportCount() << " airport(s).</p>"
          << "<div class='scroll'><table>" << kAirportHeadNoCount;
        for (const Airport* ap : db.airportsByIataOrder()) airportRowNoCount(b, *ap);
        b << "</table></div>" << backLink();
        res.set_content(page("All airports", b.str()), "text/html");
    });

    // 2.3 Get ID: student ID # and name.
    svr.Get("/get-id", [](const httplib::Request&, httplib::Response& res) {
        std::ostringstream b;
        b << "<h2>Get ID</h2><table>"
          << "<tr><th>Student name</th><td>" << htmlEscape(studentName()) << "</td></tr>"
          << "<tr><th>De Anza student ID</th><td>"
          << (studentId().empty() ? std::string("(not yet configured)")
                                  : htmlEscape(studentId()))
          << "</td></tr></table>" << backLink();
        res.set_content(page("Get ID", b.str()), "text/html");
    });

    // C.1 Get Owner: student ID (site ownership).
    svr.Get("/get-owner", [](const httplib::Request&, httplib::Response& res) {
        std::ostringstream b;
        b << "<h2>Site owner</h2><p>This site belongs to <b>" << htmlEscape(studentName())
          << "</b>, De Anza student ID <b>"
          << (studentId().empty() ? std::string("(not yet configured)")
                                  : htmlEscape(studentId()))
          << "</b>.</p>" << backLink();
        res.set_content(page("Get Owner", b.str()), "text/html");
    });

    // C.2 Get Code: scrollable window with all of this site's C++ source.
    svr.Get("/get-code", [&sourcePath](const httplib::Request&, httplib::Response& res) {
        std::string src = readTextFile(sourcePath);
        std::ostringstream b;
        b << "<h2>Get Code — the C++ source of this web service</h2>";
        if (src.empty()) {
            b << "<p>Source file not available on the server right now.</p>";
        } else {
            b << "<div class='scroll' style='max-height:75vh'><pre class='code'>"
              << htmlEscape(src) << "</pre></div>";
        }
        b << backLink();
        res.set_content(page("Get Code", b.str()), "text/html");
    });

    // C.3 One-hop report: S->X->D direct route pairs ordered by total miles,
    // plus a map of the top 5 intermediate cities by # of route pairs.
    svr.Get("/one-hop", [&db](const httplib::Request& req, httplib::Response& res) {
        const Airport* s = db.airportByIata(req.get_param_value("from"));
        const Airport* d = db.airportByIata(req.get_param_value("to"));
        std::ostringstream b;
        if (!s || !d) {
            b << "<h2>Not found</h2><p>One or both airport codes were not found.</p>";
        } else {
            auto hops = db.oneHop(*s, *d);
            b << "<h2>One-hop routes: " << htmlEscape(s->iata) << " (" << htmlEscape(s->city)
              << ") → " << htmlEscape(d->iata) << " (" << htmlEscape(d->city) << ")</h2>";
            if (db.hasDirectRoute(*s, *d))
                b << "<p class='note'>Note: a nonstop route also exists for this pair.</p>";
            if (hops.empty()) {
                b << "<p>No one-hop route pairs (both legs nonstop) connect these airports.</p>";
            } else {
                b << "<p class='note'>" << hops.size()
                  << " intermediate airport(s), ordered by total air miles.</p>"
                  << "<div class='scroll'><table>"
                  << "<tr><th>Via</th><th>City</th><th>Country</th><th>Route pairs</th>"
                  << "<th>Total air miles</th><th>Airlines " << htmlEscape(s->iata)
                  << "→via</th><th>Airlines via→" << htmlEscape(d->iata) << "</th></tr>";
                for (const auto& h : hops) {
                    b << "<tr><td>" << htmlEscape(h.via->iata) << "</td><td>"
                      << htmlEscape(h.via->city) << "</td><td>" << htmlEscape(h.via->country)
                      << "</td><td>" << h.pairs << "</td><td>"
                      << static_cast<long>(h.miles + 0.5) << "</td><td>"
                      << htmlEscape(h.leg1Airlines) << "</td><td>"
                      << htmlEscape(h.leg2Airlines) << "</td></tr>";
                }
                b << "</table></div>";

                // Top 5 intermediate cities by # of route pairs -> map.
                std::vector<const AirTravelDB::Hop*> byPairs;
                for (const auto& h : hops) byPairs.push_back(&h);
                std::sort(byPairs.begin(), byPairs.end(),
                          [](const AirTravelDB::Hop* a, const AirTravelDB::Hop* c) {
                              return a->pairs != c->pairs ? a->pairs > c->pairs
                                                           : a->miles < c->miles;
                          });
                if (byPairs.size() > 5) byPairs.resize(5);
                b << "<h2>Map — top " << byPairs.size() << " intermediate cities by # of routes</h2>"
                  << "<div id='map' style='height:420px;border:1px solid #c9d4dd'></div>"
                  << "<link rel='stylesheet' href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
                  << "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
                  << "<script>window.addEventListener('load',function(){var map=L.map('map',{center:["
                  << num((s->lat + d->lat) / 2) << "," << num((s->lon + d->lon) / 2)
                  << "],zoom:2,minZoom:1,scrollWheelZoom:false});"
                  << "L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',"
                  << "{attribution:'© OpenStreetMap contributors'}).addTo(map);"
                  << "var pts=[];function mark(lat,lon,label){var m=L.marker([lat,lon]).addTo(map);"
                  << "m.bindPopup(label);pts.push([lat,lon]);}"
                  << "mark(" << num(s->lat) << "," << num(s->lon) << ",'"
                  << htmlEscape(s->iata) << " — " << htmlEscape(s->city) << " (start)');"
                  << "mark(" << num(d->lat) << "," << num(d->lon) << ",'"
                  << htmlEscape(d->iata) << " — " << htmlEscape(d->city) << " (destination)');";
                for (const auto* h : byPairs) {
                    b << "mark(" << num(h->via->lat) << "," << num(h->via->lon) << ",'"
                      << htmlEscape(h->via->iata) << " — " << htmlEscape(h->via->city)
                      << " (" << h->pairs << " route pairs, "
                      << static_cast<long>(h->miles + 0.5) << " mi total)');"
                      << "L.polyline([[" << num(s->lat) << "," << num(s->lon) << "],["
                      << num(h->via->lat) << "," << num(h->via->lon) << "],["
                      << num(d->lat) << "," << num(d->lon)
                      << "]],{color:'#0b6fa4',weight:1.5,opacity:.6}).addTo(map);";
                }
                b << "map.fitBounds(pts,{padding:[30,30]});map.invalidateSize();"
                  << "setTimeout(function(){map.invalidateSize();map.fitBounds(pts,{padding:[30,30]});},500);});</script>";
                // Same top-5 data as a plain table, so the information is
                // readable even if the map tiles or scripts do not load.
                b << "<div class='scroll'><table>"
                  << "<tr><th>Rank</th><th>Via</th><th>City</th><th>Country</th>"
                  << "<th>Route pairs</th><th>Total air miles</th></tr>";
                int rank = 1;
                for (const auto* h : byPairs) {
                    b << "<tr><td>" << rank++ << "</td><td>" << htmlEscape(h->via->iata)
                      << "</td><td>" << htmlEscape(h->via->city) << "</td><td>"
                      << htmlEscape(h->via->country) << "</td><td>" << h->pairs
                      << "</td><td>" << static_cast<long>(h->miles + 0.5)
                      << "</td></tr>";
                }
                b << "</table></div>";
            }
        }
        b << backLink();
        res.set_content(page("One-hop report", b.str()), "text/html");
    });

    // Sample-query helper: airlines flying INTO an airport (IATA or city).
    svr.Get("/airlines-into", [&db](const httplib::Request& req, httplib::Response& res) {
        std::string q = req.get_param_value("airport");
        const Airport* ap = db.findAirport(q);
        std::ostringstream b;
        if (!ap) {
            b << "<h2>Not found</h2><p>No airport matching '" << htmlEscape(q) << "'.</p>";
        } else {
            auto airlines = db.airlinesInto(*ap);
            b << "<h2>Airlines flying into " << htmlEscape(ap->city) << " ("
              << htmlEscape(ap->iata) << ")</h2><p>" << htmlEscape(ap->name) << "</p><ul>";
            for (const Airline* al : airlines) {
                b << "<li>" << htmlEscape(al->name) << " (" << htmlEscape(al->iata) << ")</li>";
            }
            b << "</ul><p>" << airlines.size() << " airline(s).</p>";
        }
        b << backLink();
        res.set_content(page("Airlines into airport", b.str()), "text/html");
    });

    // Sample-query helper: top N cities an airline flies into.
    svr.Get("/top-cities", [&db](const httplib::Request& req, httplib::Response& res) {
        std::string code = req.get_param_value("airline");
        int n = 3;
        if (req.has_param("n")) n = std::max(1, std::min(25, toInt(req.get_param_value("n"), 3)));
        const Airline* al = db.airlineByIata(code);
        std::ostringstream b;
        if (!al) {
            b << "<h2>Not found</h2><p>No airline with code '" << htmlEscape(code) << "'.</p>";
        } else {
            auto top = db.topCitiesInto(*al, static_cast<size_t>(n));
            b << "<h2>Top " << top.size() << " cities " << htmlEscape(al->name) << " ("
              << htmlEscape(al->iata) << ") flies into</h2>"
              << "<table><tr><th>City</th><th>Airport</th><th>Routes in</th></tr>";
            for (const auto& tc : top) {
                b << "<tr><td>" << htmlEscape(tc.airport->city) << "</td><td>"
                  << htmlEscape(tc.airport->iata) << "</td><td>" << tc.count << "</td></tr>";
            }
            b << "</table>";
        }
        b << backLink();
        res.set_content(page("Top cities", b.str()), "text/html");
    });

    // Sample-query helper: great-circle distance between two airports.
    svr.Get("/distance", [&db](const httplib::Request& req, httplib::Response& res) {
        const Airport* a = db.airportByIata(req.get_param_value("from"));
        const Airport* c = db.airportByIata(req.get_param_value("to"));
        std::ostringstream b;
        if (!a || !c) {
            b << "<h2>Not found</h2><p>One or both airport codes were not found.</p>";
        } else {
            double mi = AirTravelDB::distanceMiles(*a, *c);
            b << "<h2>" << htmlEscape(a->iata) << " to " << htmlEscape(c->iata) << "</h2>"
              << "<p>" << htmlEscape(a->city) << " (" << htmlEscape(a->name) << ") to "
              << htmlEscape(c->city) << " (" << htmlEscape(c->name) << ")</p>"
              << "<p><b>" << static_cast<long>(mi + 0.5) << " miles</b> ("
              << static_cast<long>(mi * 1.60934 + 0.5) << " km), great-circle distance.</p>";
        }
        b << backLink();
        res.set_content(page("Distance", b.str()), "text/html");
    });

    int port = 8080; // hosts like Render/Fly assign $PORT instead
    if (const char* e = std::getenv("PORT")) {
        int p = toInt(e, 0);
        if (p > 0) port = p;
    }
    std::cout << "Listening on port " << port << "\n";
    svr.listen("0.0.0.0", port);
    return 0;
}
