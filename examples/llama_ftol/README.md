# Running docker

docker run -it --ipc=host  -v ~/astra-sim-hybrid-parallelism:/app/astra-sim  astra


rootFetchRing in system.json

Modify this:
int AstraSim::g_root_fetch_mf = 2;
RootFetchRing::QuarterPlacement AstraSim::g_root_fetch_quarter =
    RootFetchRing::QuarterPlacement::Q1;

397070766

419179950