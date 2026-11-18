Public synthetic fixtures, 1920×1080 BGRX/BGRA on black.
Each result uses 5 fresh processes, 30 warmup frames, and 600 measured frames per process.
Wall timing includes compositing. Render FPS excludes frame clearing and process startup.
The Mangetsu legacy path uses the harness's scalar BGR compositor.
The first render follows setup in a fresh process. The OS font and file caches may still be warm.
Range is the minimum to maximum of the five per-trial median frame times.

| Workload | Engine | Mean ms | Median ms | Range ms | p95 ms | FPS | First frame ms | CPU ms | Peak MiB |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| idle | Original VSFilterMod r5.2.7 | 0.0001 | 0.0001 | 0.0001–0.0001 | 0.0001 | 10791366.9 | 0.0017 | 0.0 | 16.9 |
| idle | Oborodzuki baseline | 0.0001 | 0.0001 | 0.0001–0.0001 | 0.0001 | 11342155.0 | 0.0039 | 0.0 | 17.2 |
| idle | Mangetsu legacy | 0.0014 | 0.0014 | 0.0013–0.0017 | 0.0016 | 725163.2 | 0.0370 | 0.0 | 21.9 |
| idle | Mangetsu RGBA | 0.0014 | 0.0014 | 0.0013–0.0014 | 0.0016 | 711743.8 | 0.0454 | 0.0 | 22.4 |
| idle | Oborodzuki candidate | 0.0001 | 0.0001 | 0.0001–0.0001 | 0.0001 | 11428571.4 | 0.0045 | 0.0 | 17.2 |
| static | Original VSFilterMod r5.2.7 | 0.1287 | 0.1283 | 0.1273–0.1288 | 0.1305 | 7768.6 | 3.9760 | 78.1 | 16.9 |
| static | Oborodzuki baseline | 0.3563 | 0.3528 | 0.3473–0.3559 | 0.3805 | 2806.7 | 4.1947 | 218.8 | 17.2 |
| static | Mangetsu legacy | 0.4484 | 0.4466 | 0.4447–0.4480 | 0.4569 | 2230.2 | 5.7850 | 265.6 | 21.9 |
| static | Mangetsu RGBA | 0.4809 | 0.4651 | 0.4597–0.4794 | 0.5667 | 2079.3 | 5.8925 | 296.9 | 22.4 |
| static | Oborodzuki candidate | 0.3540 | 0.3498 | 0.3478–0.3713 | 0.3738 | 2824.6 | 4.3919 | 203.1 | 17.2 |
| animation | Original VSFilterMod r5.2.7 | 6.3416 | 6.6166 | 6.5669–6.7086 | 7.2936 | 157.7 | 6.2607 | 3796.9 | 17.3 |
| animation | Oborodzuki baseline | 6.8020 | 7.1273 | 7.0219–7.2536 | 7.8786 | 147.0 | 6.9203 | 4109.4 | 17.6 |
| animation | Mangetsu legacy | 0.9603 | 0.9298 | 0.9166–0.9369 | 1.2973 | 1041.3 | 6.5233 | 578.1 | 67.5 |
| animation | Mangetsu RGBA | 0.7322 | 0.6861 | 0.6572–0.6916 | 1.0481 | 1365.8 | 6.6434 | 453.1 | 71.5 |
| animation | Oborodzuki candidate | 6.8588 | 7.1437 | 7.0366–7.4053 | 7.9595 | 145.8 | 6.6856 | 4140.6 | 17.7 |
| seek | Original VSFilterMod r5.2.7 | 6.5803 | 6.5435 | 6.2642–6.8793 | 7.2378 | 152.0 | 6.1287 | 3937.5 | 17.3 |
| seek | Oborodzuki baseline | 7.1338 | 7.0995 | 6.6421–7.4714 | 7.7958 | 140.2 | 6.8377 | 4343.8 | 17.7 |
| seek | Mangetsu legacy | 1.1185 | 1.1107 | 1.1056–1.1987 | 1.3437 | 894.1 | 6.4569 | 687.5 | 144.3 |
| seek | Mangetsu RGBA | 0.9455 | 0.9059 | 0.8723–0.9756 | 1.1756 | 1057.6 | 6.5695 | 515.6 | 148.9 |
| seek | Oborodzuki candidate | 7.4095 | 7.3743 | 6.7476–7.3892 | 8.0999 | 135.0 | 6.8847 | 4437.5 | 17.7 |
| heavy | Original VSFilterMod r5.2.7 | 53.6292 | 53.9871 | 53.2567–55.9331 | 59.1277 | 18.6 | 47.6614 | 32203.1 | 25.4 |
| heavy | Oborodzuki baseline | 60.5013 | 60.7343 | 60.1277–64.1827 | 67.8484 | 16.5 | 52.0255 | 36328.1 | 25.7 |
| heavy | Mangetsu legacy | 9.0049 | 9.1275 | 9.0529–9.6158 | 11.1094 | 111.1 | 13.2113 | 5406.2 | 158.5 |
| heavy | Mangetsu RGBA | 7.7289 | 7.7430 | 7.5305–8.2616 | 9.4239 | 129.4 | 13.7927 | 4703.1 | 168.8 |
| heavy | Oborodzuki candidate | 60.7604 | 60.9605 | 59.6096–63.5021 | 67.8372 | 16.5 | 50.7946 | 36437.5 | 25.7 |

Candidate latency relative to each reference. Positive savings indicate lower latency.

| Workload | Reference | Speedup | Latency savings |
|---|---|---:|---:|
| idle | Original VSFilterMod r5.2.7 | 1.00× | +0.0% |
| idle | Oborodzuki baseline | 1.00× | +0.0% |
| idle | Mangetsu legacy | 14.00× | +92.9% |
| idle | Mangetsu RGBA | 14.00× | +92.9% |
| static | Original VSFilterMod r5.2.7 | 0.37× | -172.6% |
| static | Oborodzuki baseline | 1.01× | +0.9% |
| static | Mangetsu legacy | 1.28× | +21.7% |
| static | Mangetsu RGBA | 1.33× | +24.8% |
| animation | Original VSFilterMod r5.2.7 | 0.93× | -8.0% |
| animation | Oborodzuki baseline | 1.00× | -0.2% |
| animation | Mangetsu legacy | 0.13× | -668.3% |
| animation | Mangetsu RGBA | 0.10× | -941.3% |
| seek | Original VSFilterMod r5.2.7 | 0.89× | -12.7% |
| seek | Oborodzuki baseline | 0.96× | -3.9% |
| seek | Mangetsu legacy | 0.15× | -563.9% |
| seek | Mangetsu RGBA | 0.12× | -714.0% |
| heavy | Original VSFilterMod r5.2.7 | 0.89× | -12.9% |
| heavy | Oborodzuki baseline | 1.00× | -0.4% |
| heavy | Mangetsu legacy | 0.15× | -567.9% |
| heavy | Mangetsu RGBA | 0.13× | -687.3% |

Candidate versus pinned Oborodzuki RGB frame hashes: all sampled frames match.
