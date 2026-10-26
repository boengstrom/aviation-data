# Aviation Data Front End — CIS 51 Vibe Coding Assignment

C++ (cpp-httplib) web service over the provided OpenFlights data files.
Builds and runs locally on **port 8080**.

## Run it

```sh
./start.sh            # builds if needed, then serves http://localhost:8080
```

Data files live in `data/` (SB-airlines.txt, SB-airports.txt, SB-routes.txt).
They are the official copies Bo uploaded on 2026-10-23.

## Pages

| URL | What it does |
|---|---|
| `/` | Home: counts + route map (pick an airline, see its routes) + a form for every feature |
| `/airline-routes?code=AA` | JSON: an airline's airports + route segments (feeds the homepage map) |
| `/airline?code=AA` | Full airline entity (all elements) |
| `/airport?code=SFO` | Full airport entity (all elements) |
| `/airline-airports?code=AA` | Airports reached by an airline, ordered by # routes to/from |
| `/airport-airlines?code=DEN` | Airlines serving an airport, ordered by # routes to/from |
| `/airlines` | All airlines ordered by IATA |
| `/airports` | All airports ordered by IATA |
| `/one-hop?from=SFO&to=ORD` | S→X→D direct pairs by total miles + map of top 5 intermediate cities |
| `/get-id` · `/get-owner` | Owner name + De Anza student ID |
| `/get-code` | This site's full C++ source, scrollable |
| `/airlines-into`, `/top-cities`, `/distance` | Sample-query helpers |
| `/health` | `{"ok":true}` |

## To finish

1. Put Bo's De Anza student ID in `airline_service.cpp` (`kStudentId`,
   top of file) — or start with `STUDENT_ID=... ./start.sh`.
2. For Kleinman's review: either run it during office hours with an
   ngrok tunnel to port 8080, or deploy (GitHub + Docker + Render = extra credit).

Files: `airline_service.cpp` (current), `airline_service_stage1.cpp.bak`
(stage 1), `starter-main.cpp.bak` (original Oct 13 starter).
